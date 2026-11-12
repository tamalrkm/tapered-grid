// In-RAM head-to-head: values-only array addressed by rank vs key-storing maps.
//
// usage: bench <mode> B d M seed [patterns] [rotate]
//   rankonly            index arithmetic only, no memory access
//   tapered-ram         array in anonymous memory, transparent huge pages
//   tapered-ram-nohuge  same, 4 KiB pages
//   tapered             array in a tg::Table file mapping (memo.tg in cwd)
//   tabled              the 2015 node-count index (first coordinate pinned)
//   oahash[-nohuge]     hand-rolled open addressing, inline u64 keys
//   absl                absl::flat_hash_map<u64,float>, reserve(N)
//   boost               boost::unordered_flat_map<u64,float>, reserve(N)
//   hash                std::unordered_map<u64,float>, reserve(N)
// Suffixes, combinable:
//   +pin  queries with the first coordinate pinned to 1, as the 2015 index
//         requires; separates its query domain from its index arithmetic
//   +two  the two-read form of the rank, the library's first implementation
//   +pre  addresses computed before the clock starts: the memory reads alone
//
// patterns is a comma list of access patterns timed in this one process, after
// one build and one untimed pass over the queries (default ind). Without that
// pass, whichever pattern ran first paid the cold start. rotate, the repetition
// number, rotates their order as well.
//   ind    independent lookups: the stream is fixed, so lookups may overlap;
//          the time per lookup is reciprocal throughput
//   dep    dependent lookups: the same stream, but each query's address depends
//          on the previous answer, so lookups cannot overlap; this is latency
//   batch  independent lookups, 32 at a time: addresses first, each prefetched,
//          then the reads (tg::Table::get_many, or the same loop for a hash table)
//   batch:W  the same with batch size W (array only)
//
// One query stream (xorshift, sorted), identical across modes, so checksums
// must agree. Prints one JSON line per pattern. Build flags: see Makefile.
#include "common.hpp"
#include "tg.hpp"
#include <type_traits>
#include <unordered_map>
#include <sys/mman.h>
#if __has_include(<absl/container/flat_hash_map.h>)
#include <absl/container/flat_hash_map.h>
#define HAVE_ABSL 1
#endif
#if __has_include(<boost/unordered/unordered_flat_map.hpp>)
#include <boost/unordered/unordered_flat_map.hpp>
#define HAVE_BOOST 1
#endif

// ---- the 2015 table-based index (Eq. 2 + Eq. 3), via prefix sums ----------
static u64 V[70][70], PV[70][70];
static void build_tables(int B, int d) {
    for (int b = 0; b <= B; b++) V[1][b] = 0;
    for (int b = 1; b <= B; b++) V[2][b] = 1;
    for (int i = 3; i <= d; i++)
        for (int b = 1; b <= B; b++) { u64 s = 0; for (int j = b; j <= B; j++) s += V[i-1][j]; V[i][b] = s; }
    for (int i = 1; i <= d; i++) { PV[i][0] = 0; for (int b = 1; b <= B; b++) PV[i][b] = PV[i][b-1] + V[i][b]; }
}
TG_ALWAYS_INLINE u64 rank_tabled(const int* c, int d, int B) {
    (void)B; u64 r = 0;
    for (int i = 1; i <= d - 1; i++) { int row = d + 1 - i, a = c[i-1], b = c[i]; r += PV[row][b-1] - PV[row][a-1]; }
    return r;
}

// The rank as first implemented: both terms of each telescoping pair of
//   rank(m) = sum_{i=1}^{d} [ C(B-m_{i-1}+d-i+1, d-i+1) - C(B-m_i+d-i+1, d-i+1) ],
// m_0 = 1. Same value as tg::rank, about twice the instructions per coordinate.
TG_ALWAYS_INLINE u64 rank_two(const int* m, int d, int B, const tg::Pascal& P) {
    int prev = 1; u64 r = 0;
    for (int i = 1; i <= d; i++) {
        int mi = m[i-1], k = d - i;
        r += P(B - prev + k + 1, k + 1) - P(B - mi + k + 1, k + 1);
        prev = mi;
    }
    return r;
}

// ---- hand-rolled open addressing: 16-byte {key,value} slots, linear probing,
// splitmix64, power-of-two capacity with load <= 7/8 ------------------------
struct OASlot { u64 key; float val; };
struct OATable {
    OASlot* s; u64 mask, cap; static const u64 EMPTY = ~0ull;
    OATable(u64 n, bool nohuge) {
        cap = 1; while (double(n) / double(cap) > 0.875) cap <<= 1; mask = cap - 1;
        size_t bytes = cap * sizeof(OASlot);
        s = (OASlot*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        madvise(s, bytes, nohuge ? MADV_NOHUGEPAGE : MADV_HUGEPAGE);
        for (u64 i = 0; i < cap; i++) s[i].key = EMPTY;
    }
    ~OATable() { munmap(s, cap * sizeof(OASlot)); }
    void insert(u64 k, float v) { u64 i = mix64(k) & mask; while (s[i].key != EMPTY && s[i].key != k) i = (i + 1) & mask; s[i].key = k; s[i].val = v; }
    float from(u64 i, u64 k) const { while (s[i].key != k) i = (i + 1) & mask; return s[i].val; }
    float get(u64 k) const { return from(mix64(k) & mask, k); }
};

template <class Ins>
static void enumerate(int B, int d, Ins ins) {        // every key, lex order
    std::vector<int> c(d, 1);
    do ins(c.data()); while (tg::next(c.data(), d, B));
}

static constexpr long BATCH = 32;
#define INL __attribute__((always_inline))          // every structure's lookup inlined alike

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s mode B d M seed [patterns] [rotate]\n", argv[0]); return 1; }
    std::string mode = argv[1];
    auto has = [&](const char* sfx) { auto p = mode.find(sfx); if (p == std::string::npos) return false; mode.erase(p, std::strlen(sfx)); return true; };
    const bool pin = has("+pin"), two = has("+two"), pre = has("+pre");
    const std::string mode_full = argv[1];
    int B = atoi(argv[2]), d = atoi(argv[3]); long M = atol(argv[4]); u64 seed = strtoull(argv[5], 0, 10);
    std::vector<std::string> pats;
    { std::string p = argc > 6 ? argv[6] : "ind"; size_t i = 0;
      while (i <= p.size()) { size_t j = p.find(',', i); if (j == std::string::npos) j = p.size(); pats.push_back(p.substr(i, j - i)); i = j + 1; }
      if (argc > 7) std::rotate(pats.begin(), pats.begin() + atol(argv[7]) % (long)pats.size(), pats.end()); }
    tg::Shape sh{B, d};
    if (!sh.valid()) { std::fprintf(stderr, "invalid shape\n"); return 1; }
    const tg::Pascal& P = tg::pascal();
    const u64 N = sh.size();

    std::vector<int> Q((size_t)M * d);
    { u64 s = seed | 1; for (long q = 0; q < M; q++) gen_query(s, d, B, &Q[(size_t)q * d]); }
    if (pin) for (long q = 0; q < M; q++) Q[(size_t)q * d] = 1;
    const double qbuf_mib = double(Q.size() * sizeof(int)) / 1048576.0;

    // Every build generates each key and computes the same payload from it, so build
    // times differ only by what each structure does with the pair.
    auto payload = [&](const int* m, int dd) { return value_of(tg::rank(m, dd, B, P)); };
    double build_s = 0, struct_mib = 0; u64 capacity = 0;
    long batch_w = 0;                                   // from pattern batch:W
    auto emit = [&](const std::string& pat, double lat_ns, double checksum) {
        J("bench").s("mode", mode_full + (pat == "ind" ? "" : "+" + pat)).s("base_mode", mode).s("pattern", pat)
            .i("pin", pin || mode == "tabled").i("two", two).i("pre", pre).i("B", B).i("d", d).u("N", N).i("M", M).u("seed", seed)
            .f("build_s", build_s).f("query_ns", lat_ns).f("peak_rss_mib", peak_rss_mib())
            .f("qbuf_mib", qbuf_mib).f("struct_mib", struct_mib).f("anon_huge_mib", anon_huge_mib()).u("capacity", capacity)
            .d("checksum", checksum).s("thp", thp_mode()).emit();
    };
    // get: answer one query. batched: answer all of Q in batches, or nullptr.
    auto measure = [&](auto get, auto batched) {
        double warm = 0;
        for (long q = 0; q < M; q++) warm += get(&Q[(size_t)q * d]);
        std::fprintf(stderr, "warm-up pass checksum %.3f\n", warm);
        for (const std::string& pat : pats) {
            double checksum = 0;
            auto t1 = clk::now();
            if (pat == "ind") {
                for (long q = 0; q < M; q++) checksum += get(&Q[(size_t)q * d]);
            } else if (pat == "dep") {
                // Same queries in the same order, but query q's address depends on
                // answer q-1 through v*0.0 (not foldable without -ffast-math), so
                // the lookups cannot overlap. An index that is a function of the
                // previous one alone would fall into a short cycle and hit cache.
                double v = 0;
                for (long q = 0; q < M; q++) {
                    size_t j = (size_t)q + (size_t)(v * 0.0);
                    v = get(&Q[j * d]); checksum += v;
                }
            } else if constexpr (!std::is_same_v<decltype(batched), std::nullptr_t>) {
                batch_w = pat.size() > 6 ? atol(pat.c_str() + 6) : 0;
                batched(checksum);
            } else {
                std::fprintf(stderr, "no batched path for this mode\n"); std::exit(1);
            }
            emit(pat, secs_since(t1) * 1e9 / M, checksum);
        }
    };

    if (mode == "rankonly") {
        u64 acc = 0; auto t1 = clk::now();
        if (two) for (long q = 0; q < M; q++) acc += rank_two(&Q[(size_t)q * d], d, B, P);
        else     for (long q = 0; q < M; q++) acc += tg::rank(&Q[(size_t)q * d], d, B, P);
        emit("ind", secs_since(t1) * 1e9 / M, double(acc & 0xFFFFFF));
    } else if (mode == "tapered-ram" || mode == "tapered-ram-nohuge" || mode == "tapered") {
        const bool file = mode == "tapered";
        auto t0 = clk::now();
        auto t = file ? tg::Table<float>::create("memo.tg", sh) : tg::Table<float>::anonymous(sh, mode == "tapered-ram");
        t.fill(payload);
        if (file) t.sync(false);
        build_s = secs_since(t0); struct_mib = N * 4.0 / 1048576.0;
        const float* a = t.data();
        if (pre) {
            std::vector<u64> R(M);
            for (long q = 0; q < M; q++) R[q] = tg::rank(&Q[(size_t)q * d], d, B, P);
            const u64* rp = R.data();
            { double warm = 0; for (long q = 0; q < M; q++) warm += a[rp[q]];
              std::fprintf(stderr, "warm-up pass checksum %.3f\n", warm); }
            for (const std::string& pat : pats) {
                double checksum = 0, v = 0; auto t1 = clk::now();
                if (pat == "dep") for (long q = 0; q < M; q++) { size_t j = (size_t)q + (size_t)(v * 0.0); v = a[rp[j]]; checksum += v; }
                else              for (long q = 0; q < M; q++) checksum += a[rp[q]];
                emit(pat, secs_since(t1) * 1e9 / M, checksum);
            }
        } else if (two) {
            measure([&](const int* m) INL { return a[rank_two(m, d, B, P)]; }, [&](double& cs) {
                u64 r[BATCH];
                for (long q = 0; q < M; q += BATCH) {
                    const long n = std::min(BATCH, M - q);
                    for (long i = 0; i < n; i++) { r[i] = rank_two(&Q[(size_t)(q + i) * d], d, B, P); __builtin_prefetch(a + r[i]); }
                    for (long i = 0; i < n; i++) cs += a[r[i]];
                } });
        } else {
            measure([&](const int* m) INL { return a[tg::rank(m, d, B, P)]; }, [&](double& cs) {
                if (batch_w) {
                    std::vector<u64> r(batch_w);
                    for (long q = 0; q < M; q += batch_w) {
                        const long n = std::min(batch_w, M - q);
                        for (long i = 0; i < n; i++) { r[i] = tg::rank(&Q[(size_t)(q + i) * d], d, B, P); __builtin_prefetch(a + r[i]); }
                        for (long i = 0; i < n; i++) cs += a[r[i]];
                    }
                    return;
                }
                float out[1024];
                for (long q = 0; q < M; q += 1024) {
                    const long n = std::min(1024L, M - q);
                    t.get_many(&Q[(size_t)q * d], (size_t)n, out);
                    for (long i = 0; i < n; i++) cs += out[i];
                } });
        }
    } else if (mode == "tabled") {
        build_tables(B, d);
        for (long q = 0; q < M; q++) Q[(size_t)q * d] = 1;      // its domain: x_1 pinned
        { u64 s = seed | 7;
          for (int k = 0; k < 20000; k++) { int c[64]; gen_query(s, d, B, c); c[0] = 1;
              if (rank_tabled(c, d, B) != tg::rank(c, d, B, P)) { std::fprintf(stderr, "MISMATCH\n"); return 2; } } }
        auto t0 = clk::now();
        auto t = tg::Table<float>::create("memo.tg", sh);
        t.fill(payload);
        t.sync(false);
        build_s = secs_since(t0); struct_mib = N * 4.0 / 1048576.0;
        const float* a = t.data();
        measure([&](const int* m) INL { return a[rank_tabled(m, d, B)]; }, nullptr);
    } else if (mode == "oahash" || mode == "oahash-nohuge") {
        Packer pack(B, d);
        auto t0 = clk::now();
        OATable T(N, mode == "oahash-nohuge");
        enumerate(B, d, [&](const int* m) { T.insert(pack(m, d), value_of(tg::rank(m, d, B, P))); });
        build_s = secs_since(t0); capacity = T.cap; struct_mib = T.cap * sizeof(OASlot) / 1048576.0;
        measure([&](const int* m) INL { return T.get(pack(m, d)); }, [&](double& cs) {
            u64 k[BATCH], h[BATCH];
            for (long q = 0; q < M; q += BATCH) {
                const long n = std::min(BATCH, M - q);
                for (long i = 0; i < n; i++) { k[i] = pack(&Q[(size_t)(q + i) * d], d); h[i] = mix64(k[i]) & T.mask; __builtin_prefetch(&T.s[h[i]]); }
                for (long i = 0; i < n; i++) cs += T.from(h[i], k[i]);
            } });
#ifdef HAVE_ABSL
    } else if (mode == "absl") {
        Packer pack(B, d);
        auto t0 = clk::now();
        absl::flat_hash_map<u64, float> mp; mp.reserve(N);
        enumerate(B, d, [&](const int* m) { mp.emplace(pack(m, d), value_of(tg::rank(m, d, B, P))); });
        build_s = secs_since(t0); capacity = mp.capacity();
        struct_mib = capacity * (sizeof(std::pair<const u64, float>) + 1.0) / 1048576.0;
        measure([&](const int* m) INL { return mp.find(pack(m, d))->second; }, [&](double& cs) {
            u64 k[BATCH];
            for (long q = 0; q < M; q += BATCH) {
                const long n = std::min(BATCH, M - q);
                for (long i = 0; i < n; i++) { k[i] = pack(&Q[(size_t)(q + i) * d], d); mp.prefetch(k[i]); }
                for (long i = 0; i < n; i++) cs += mp.find(k[i])->second;
            } });
#endif
#ifdef HAVE_BOOST
    } else if (mode == "boost") {
        Packer pack(B, d);
        auto t0 = clk::now();
        boost::unordered_flat_map<u64, float> mp; mp.reserve(N);
        enumerate(B, d, [&](const int* m) { mp.emplace(pack(m, d), value_of(tg::rank(m, d, B, P))); });
        build_s = secs_since(t0); capacity = mp.bucket_count();
        struct_mib = capacity * (sizeof(std::pair<const u64, float>) + 16.0 / 15.0) / 1048576.0;
        measure([&](const int* m) INL { return mp.find(pack(m, d))->second; }, nullptr);
#endif
    } else if (mode == "hash") {
        Packer pack(B, d);
        auto t0 = clk::now();
        std::unordered_map<u64, float> mp; mp.reserve(N);
        enumerate(B, d, [&](const int* m) { mp[pack(m, d)] = value_of(tg::rank(m, d, B, P)); });
        build_s = secs_since(t0); capacity = mp.bucket_count();
        measure([&](const int* m) INL { return mp.find(pack(m, d))->second; }, nullptr);
    } else {
        std::fprintf(stderr, "unknown or unavailable mode '%s'\n", mode.c_str()); return 1;
    }
    std::remove("memo.tg");
    return 0;
}
