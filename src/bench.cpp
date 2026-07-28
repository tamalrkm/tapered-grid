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

static inline u64 pack(const int* cols, int d) {      // 4 bits/col (B<=16)
    u64 k = 0; for (int i = 0; i < d; i++) k |= u64(cols[i]-1) << (4*i); return k;
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

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s tapered|hash B d M seed\n", argv[0]); return 1; }
    std::string mode = argv[1];
    int B = atoi(argv[2]), d = atoi(argv[3]); long M = atol(argv[4]); u64 seed = atoll(argv[5]);
    pascal();
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
