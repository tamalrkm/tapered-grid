// At-scale, out-of-core head-to-head for the SEA paper's central claim:
//   tapered flat array + O(d) closed-form address (mmap'd file)  vs  hash memo.
//
// The Python micro-bench let the C-level dict win because our rank was
// interpreted. Here both are compiled; the flat array is a real mmap'd file
// (out-of-core, like the paper's 77M-entry file) and the map is a real
// std::unordered_map. We measure query latency and peak RSS at a scale where
// the working set >> L3, so DRAM access pattern — not language overhead —
// decides it.
//
// build:  g++ -O3 -march=native -o bench bench.cpp
// run:    ./bench tapered 16 13 3000000 42
//         ./bench hash    16 13 3000000 42
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>

using u64 = uint64_t;
using clk = std::chrono::high_resolution_clock;

static u64 C[64][64];               // Pascal's triangle
static void pascal() {
    for (int n = 0; n < 64; n++) { C[n][0] = 1;
        for (int k = 1; k <= n; k++) C[n][k] = C[n-1][k-1] + (k<=n-1?C[n-1][k]:0); }
}

// table-free O(d) lexicographic rank (hockey-stick closed form), cols 1..B
static inline u64 rank_fixed(const int* cols, int d, int B) {
    int m_prev = 1; u64 r = 0;
    for (int i = 1; i <= d; i++) {
        int mi = cols[i-1], k = d - i;
        r += C[B - m_prev + k + 1][k+1] - C[B - mi + k + 1][k+1];
        m_prev = mi;
    }
    return r;
}

// ---- base paper's TABLE-BASED index (Eq. 2 + Eq. 3): V table + prefix sums --
static u64 V[64][64], PV[64][64];
static void build_tables(int B, int d) {
    for (int b = 0; b <= B; b++) V[1][b] = 0;
    for (int b = 1; b <= B; b++) V[2][b] = 1;
    for (int i = 3; i <= d; i++)
        for (int b = 1; b <= B; b++) { u64 s = 0; for (int j = b; j <= B; j++) s += V[i-1][j]; V[i][b] = s; }
    for (int i = 1; i <= d; i++) { PV[i][0] = 0; for (int b = 1; b <= B; b++) PV[i][b] = PV[i][b-1] + V[i][b]; }
}
static inline u64 rank_tabled(const int* cols, int d, int B) {   // Eq. 3 via prefix sums
    u64 r = 0;
    for (int i = 1; i <= d-1; i++) { int row = d+1-i, a = cols[i-1], b = cols[i];
        r += PV[row][b-1] - PV[row][a-1]; }
    return r;
}

static inline float value_of(u64 r) {                 // cheap deterministic payload
    u64 h = r * 2654435761ull; return float(h & 0xFFFFFF) * (1.0f/16777216.0f);
}

// Packed key: ceil(log2 B) bits per column, so any B fits without collisions
// as long as bits*d <= 63 (the all-ones word is reserved as the open-addressing
// empty sentinel). The earlier hardcoded 4 bits/column silently collided for
// B > 16; the server N-sweep already used this generalization.
static int PACK_BITS = 4;
static void set_pack_bits(int B, int d) {
    PACK_BITS = 1; while ((1 << PACK_BITS) < B) PACK_BITS++;
    if (PACK_BITS * d > 63) { fprintf(stderr, "pack: %d bits x %d cols exceeds 63\n", PACK_BITS, d); exit(1); }
}
static inline u64 pack(const int* cols, int d) {
    u64 k = 0; for (int i = 0; i < d; i++) k |= u64(cols[i]-1) << (PACK_BITS*i); return k;
}

static long peak_rss_mb() {
    struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss / 1024;
}

// xorshift so query generation is identical & cheap across both modes
static inline u64 xs(u64& s){ s^=s<<13; s^=s>>7; s^=s<<17; return s; }
static void gen_query(u64& s, int d, int B, int* out) {
    for (int i = 0; i < d; i++) out[i] = int(xs(s) % B) + 1;
    std::sort(out, out + d);
}


// ---- open-addressing flat hash: inline 64-bit keys, linear probing ---------
// The strongest simple in-RAM baseline: one contiguous array of 16-byte
// {key,value} slots (the layout of absl::flat_hash_map<u64,float>), capacity a
// power of two chosen so that load <= 7/8 (absl's max load), splitmix64
// finalizer as the hash, linear probing. Empty slot = all-ones key, which no
// packed key can equal because set_pack_bits() enforces bits*d <= 63.
static inline u64 mix64(u64 x){ x+=0x9e3779b97f4a7c15ull; x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
    x=(x^(x>>27))*0x94d049bb133111ebull; return x^(x>>31); }
struct OASlot { u64 key; float val; };
struct OATable {
    OASlot* s; u64 mask; u64 cap;
    static const u64 EMPTY = ~0ull;
    OATable(u64 n, bool nohuge=false){ cap=1; while (double(n)/double(cap) > 0.875) cap<<=1; mask=cap-1;
        size_t bytes=cap*sizeof(OASlot);
        s=(OASlot*)mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        madvise(s,bytes, nohuge?MADV_NOHUGEPAGE:MADV_HUGEPAGE);
        for(u64 i=0;i<cap;i++) s[i].key=EMPTY; }
    inline void insert(u64 k,float v){ u64 i=mix64(k)&mask;
        while(s[i].key!=EMPTY && s[i].key!=k) i=(i+1)&mask; s[i].key=k; s[i].val=v; }
    inline float get(u64 k) const { u64 i=mix64(k)&mask;
        while(s[i].key!=k) i=(i+1)&mask; return s[i].val; }
};

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s tapered|tapered-ram|tapered-ram-nohuge|tabled|hash|oahash|oahash-nohuge|rankonly B d M seed\n", argv[0]); return 1; }
    std::string mode = argv[1];
    int B = atoi(argv[2]), d = atoi(argv[3]); long M = atol(argv[4]); u64 seed = atoll(argv[5]);
    pascal(); set_pack_bits(B, d);
    u64 N = C[B + d - 1][d];
    printf("mode=%-7s B=%d d=%d  N=%llu  M=%ld\n", mode.c_str(), B, d, (unsigned long long)N, M);

    // pre-generate queries (common to both modes; excluded from build/latency of structure)
    std::vector<int> Q((size_t)M * d);
    { u64 s = seed | 1; for (long q = 0; q < M; q++) gen_query(s, d, B, &Q[(size_t)q*d]); }

    double checksum = 0; double build_s = 0, lat_ns = 0;

    if (mode == "tapered" || mode == "tabled") {
        // both use the same mmap'd flat array; they differ ONLY in the index
        // computation: table-free closed form (tapered) vs base-paper V-table (tabled).
        bool tabled = (mode == "tabled");
        if (tabled) {
            // The base paper's Eq. (3) is correct only on its actual domain: the
            // first coordinate pinned to the minimum (x_1 = 0). Pin col[0]=1 so
            // the comparison is faithful; then tabled == table-free.
            build_tables(B, d);
            for (long q = 0; q < M; q++) Q[(size_t)q*d] = 1;
            u64 s = seed | 7;                      // verify tabled == table-free (pinned)
            for (int t = 0; t < 20000; t++) { int c[32]; gen_query(s, d, B, c); c[0] = 1;
                if (rank_tabled(c, d, B) != rank_fixed(c, d, B)) { fprintf(stderr, "MISMATCH\n"); return 2; } }
        }
        auto t0 = clk::now();
        const char* path = "memo.bin";
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (ftruncate(fd, (off_t)N * sizeof(float))) { perror("ftruncate"); return 1; }
        float* arr = (float*)mmap(nullptr, (size_t)N*sizeof(float), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        for (u64 r = 0; r < N; r++) arr[r] = value_of(r);   // fill by rank directly
        msync(arr, (size_t)N*sizeof(float), MS_SYNC);
        build_s = std::chrono::duration<double>(clk::now()-t0).count();

        // ---- query: cols -> O(d) index -> one mmap read ------------------
        auto t1 = clk::now();
        if (tabled) for (long q = 0; q < M; q++) checksum += arr[ rank_tabled(&Q[(size_t)q*d], d, B) ];
        else        for (long q = 0; q < M; q++) checksum += arr[ rank_fixed (&Q[(size_t)q*d], d, B) ];
        lat_ns = std::chrono::duration<double,std::nano>(clk::now()-t1).count() / M;
        munmap(arr, (size_t)N*sizeof(float)); close(fd);
    } else if (mode == "tapered-ram" || mode == "tapered-ram-nohuge") {
        // same closed-form index, values in anonymous memory instead of a file
        // mapping: isolates the index from the page-size/TLB effects of mmap.
        auto t0 = clk::now();
        size_t bytes=(size_t)N*sizeof(float);
        float* arr=(float*)mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        madvise(arr,bytes, mode=="tapered-ram-nohuge"?MADV_NOHUGEPAGE:MADV_HUGEPAGE);
        for (u64 r = 0; r < N; r++) arr[r] = value_of(r);
        build_s = std::chrono::duration<double>(clk::now()-t0).count();
        auto t1 = clk::now();
        for (long q = 0; q < M; q++) checksum += arr[ rank_fixed(&Q[(size_t)q*d], d, B) ];
        lat_ns = std::chrono::duration<double,std::nano>(clk::now()-t1).count() / M;
        munmap(arr,bytes);
    } else if (mode == "rankonly") {
        // index arithmetic alone: no memory access at all (sum of ranks)
        auto t1 = clk::now();
        u64 acc=0; for (long q = 0; q < M; q++) acc += rank_fixed(&Q[(size_t)q*d], d, B);
        lat_ns = std::chrono::duration<double,std::nano>(clk::now()-t1).count() / M;
        checksum = double(acc & 0xFFFFFF);
    } else if (mode == "oahash" || mode == "oahash-nohuge") {
        auto t0 = clk::now();
        OATable T(N, mode == "oahash-nohuge");
        std::vector<int> cols(d, 1);
        for (u64 done = 0; done < N; done++) {
            T.insert(pack(cols.data(), d), value_of(rank_fixed(cols.data(), d, B)));
            int i = d - 1; while (i >= 0 && cols[i] == B) i--;
            if (i < 0) break; cols[i]++; for (int j = i+1; j < d; j++) cols[j] = cols[i];
        }
        build_s = std::chrono::duration<double>(clk::now()-t0).count();
        printf("  oahash: capacity=%llu  load=%.3f  slot=%zu B  table=%.0f MB\n",
               (unsigned long long)T.cap, double(N)/double(T.cap), sizeof(OASlot),
               double(T.cap)*sizeof(OASlot)/1048576.0);
        auto t1 = clk::now();
        for (long q = 0; q < M; q++) checksum += T.get(pack(&Q[(size_t)q*d], d));
        lat_ns = std::chrono::duration<double,std::nano>(clk::now()-t1).count() / M;
        munmap(T.s, T.cap*sizeof(OASlot));
    } else {
        // ---- build a real unordered_map keyed by packed cols -------------
        auto t0 = clk::now();
        std::unordered_map<u64,float> mp; mp.reserve(N * 1.3);
        std::vector<int> cols(d, 1);
        for (u64 done = 0; done < N; done++) {               // enumerate all combos (odometer)
            mp[pack(cols.data(), d)] = value_of(rank_fixed(cols.data(), d, B));
            int i = d - 1; while (i >= 0 && cols[i] == B) i--;      // advance non-decreasing tuple
            if (i < 0) break; cols[i]++; for (int j = i+1; j < d; j++) cols[j] = cols[i];
        }
        build_s = std::chrono::duration<double>(clk::now()-t0).count();

        // ---- query: cols -> key -> hash lookup ---------------------------
        auto t1 = clk::now();
        for (long q = 0; q < M; q++) checksum += mp[ pack(&Q[(size_t)q*d], d) ];
        lat_ns = std::chrono::duration<double,std::nano>(clk::now()-t1).count() / M;
    }

    printf("  build=%.2fs  latency=%.1f ns/query  peakRSS=%ld MB  checksum=%.3f\n",
           build_s, lat_ns, peak_rss_mb(), checksum);
    return 0;
}
