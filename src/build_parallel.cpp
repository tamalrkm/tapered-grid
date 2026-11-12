// Parallel construction: values-only array (no coordination) vs concurrent
// hash builds of the same content.
//
// usage: build_parallel <mode> B d T seed [payload=cheap|pl] [storage=anon-thp|anon-4k|shm]
//                       [prefault=0|1] [shards=1024] [verify=1000000]
//
// array modes (worker j owns indices [jN/T, (j+1)N/T)):
//   array-index    slot r <- value_of(r)            payload needs only the index
//   array-unrank   slot r <- g(unrank(r))           unranks every index
//   array-odo      slot r <- g(m), m from one unrank per range + odometer
//                  (what tg::Table::fill does)
// hash modes (each worker enumerates its own index range the same way as
// array-odo, so key generation costs the same; inserts (pack(m), g(m))):
//   umap-shard     16 std::unordered_map shards, one std::mutex each, reserved
//                  (the naive standard-library baseline)
//   absl-shard     <shards> absl::flat_hash_map shards, one std::mutex each, reserved
//   boost-conc     boost::concurrent_flat_map, reserve(N)
//   oa-cas         lock-free open addressing: pre-sized 16-byte slots, linear
//                  probing, one compare-and-swap on the key per insert
// payload: cheap = payload_cols (structure cost); pl = the vector root, by Newton
// (an expensive f: build becomes f-bound).
//
// Times are split: alloc_s (mapping, reserve, slot init, optional prefault)
// and fill_s (the parallel fill); total_s = alloc_s + fill_s.
#include "common.hpp"
#include "pl.hpp"
#include "tg.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if __has_include(<absl/container/flat_hash_map.h>)
#include <absl/container/flat_hash_map.h>
#define HAVE_ABSL 1
#endif
#if __has_include(<boost/unordered/concurrent_flat_map.hpp>)
#include <boost/unordered/concurrent_flat_map.hpp>
#define HAVE_BOOST 1
#endif

static int T_ = 1;
template <class W> static void par(u64 N, W work) {
    std::vector<std::thread> ws;
    for (int j = 0; j < T_; j++) {
        u64 lo = (unsigned __int128)N * j / T_, hi = (unsigned __int128)N * (j + 1) / T_;
        ws.emplace_back([=, &work] { work(j, lo, hi); });
    }
    for (auto& w : ws) w.join();
}

struct CasSlot { std::atomic<u64> key; float val; };
static_assert(sizeof(CasSlot) == 16, "slot layout");

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s mode B d T seed [payload] [storage] [prefault] [shards] [verify]\n", argv[0]); return 1; }
    std::string mode = argv[1];
    int B = atoi(argv[2]), d = atoi(argv[3]); T_ = atoi(argv[4]); u64 seed = strtoull(argv[5], 0, 10);
    std::string payload = argc > 6 ? argv[6] : "cheap";
    std::string storage = argc > 7 ? argv[7] : "anon-thp";
    bool prefault = argc > 8 ? atoi(argv[8]) != 0 : false;
    int S = argc > 9 ? atoi(argv[9]) : 1024;
    long V = argc > 10 ? atol(argv[10]) : (payload == "pl" ? 100000 : 1000000);

    tg::Shape sh{B, d};
    if (!sh.valid()) { std::fprintf(stderr, "invalid shape\n"); return 1; }
    const u64 N = sh.size();
    Packer pack(B, d);
    tg::Spacing sp = tg::Spacing::power(B, 2.0);
    const double* s = sp.s.data();
    const bool pl_payload = payload == "pl";
    auto g = [&](const int* m) -> float { return pl_payload ? pl::payload(m, d, s) : payload_cols(m, d); };

    double alloc_s = 0, fill_s = 0, struct_mib = 0; u64 bad = 0, size_seen = 0, content_hash = 0;
    auto t0 = clk::now();

    // keys of a range, generated the way tg::Table::fill does
    auto walk_range = [&](u64 lo, u64 hi, auto visit) {
        if (lo >= hi) return;
        int m[tg::MAX_D]; tg::unrank(lo, d, B, m);
        for (u64 r = lo; r < hi; r++) { visit(r, (const int*)m); tg::next(m, d, B); }
    };
    // verification sample: V random indices, payload recomputed from unrank
    auto check = [&](auto lookup) {
        u64 st = seed | 3; int m[tg::MAX_D];
        for (long v = 0; v < V; v++) {
            u64 r = xs(st) % N; tg::unrank(r, d, B, m);
            if (tg::rank(m, d, B) != r) { bad++; continue; }
            float want = (mode == "array-index") ? value_of(r) : g(m);
            float got = lookup(r, (const int*)m);
            if (std::memcmp(&want, &got, 4) != 0) bad++;
        }
    };

    if (mode.rfind("array-", 0) == 0) {
        size_t bytes = N * sizeof(float);
        float* a = nullptr; int fd = -1; const char* shm_path = "/dev/shm/tg_build_parallel.bin";
        if (storage == "shm") {
            fd = open(shm_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || ftruncate(fd, (off_t)bytes)) { perror("shm"); return 1; }
            a = (float*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        } else {
            a = (float*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (a != MAP_FAILED) madvise(a, bytes, storage == "anon-thp" ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
        }
        if (a == MAP_FAILED) { perror("mmap"); return 1; }
        if (prefault) par(N, [&](int, u64 lo, u64 hi) {
            for (u64 r = lo; r < hi; r += 1024) ((volatile float*)a)[r] = 0.f; });
        alloc_s = secs_since(t0);
        auto t1 = clk::now();
        if (mode == "array-index")
            par(N, [&](int, u64 lo, u64 hi) { for (u64 r = lo; r < hi; r++) a[r] = value_of(r); });
        else if (mode == "array-unrank")
            par(N, [&](int, u64 lo, u64 hi) { int m[tg::MAX_D];
                for (u64 r = lo; r < hi; r++) { tg::unrank(r, d, B, m); a[r] = g(m); } });
        else if (mode == "array-odo")
            par(N, [&](int, u64 lo, u64 hi) { walk_range(lo, hi, [&](u64 r, const int* m) { a[r] = g(m); }); });
        else { std::fprintf(stderr, "unknown mode\n"); return 1; }
        fill_s = secs_since(t1);
        struct_mib = bytes / 1048576.0; size_seen = N;
        check([&](u64 r, const int*) { return a[r]; });
        content_hash = tg::hash_bytes(a, bytes);      // after the clock: same bytes for every T
        munmap(a, bytes);
        if (fd >= 0) { close(fd); unlink(shm_path); }
    } else if (mode == "umap-shard") {
        const int SS = 16;
        std::vector<std::unordered_map<u64, float>> sh_(SS); std::vector<std::mutex> mu(SS);
        for (auto& x : sh_) x.reserve(u64(N / SS * 1.3));
        alloc_s = secs_since(t0); auto t1 = clk::now();
        par(N, [&](int, u64 lo, u64 hi) { walk_range(lo, hi, [&](u64, const int* m) {
            u64 k = pack(m, d); float v = g(m); int sid = int(mix64(k) >> 60);
            std::lock_guard<std::mutex> lk(mu[sid]); sh_[sid].emplace(k, v); }); });
        fill_s = secs_since(t1);
        for (auto& x : sh_) size_seen += x.size();
        check([&](u64, const int* m) { u64 k = pack(m, d); return sh_[mix64(k) >> 60].find(k)->second; });
#ifdef HAVE_ABSL
    } else if (mode == "absl-shard") {
        int bits = 0; while ((1 << bits) < S) bits++; S = 1 << bits;
        std::vector<absl::flat_hash_map<u64, float>> sh_(S); std::vector<std::mutex> mu(S);
        par(S, [&](int, u64 lo, u64 hi) { for (u64 j = lo; j < hi; j++) sh_[j].reserve(u64(N / S * 1.1) + 16); });
        alloc_s = secs_since(t0); auto t1 = clk::now();
        auto sid_of = [&](u64 k) { return bits ? int(mix64(k) >> (64 - bits)) : 0; };
        par(N, [&](int, u64 lo, u64 hi) { walk_range(lo, hi, [&](u64, const int* m) {
            u64 k = pack(m, d); float v = g(m); int sid = sid_of(k);
            std::lock_guard<std::mutex> lk(mu[sid]); sh_[sid].emplace(k, v); }); });
        fill_s = secs_since(t1);
        u64 cap = 0; for (auto& x : sh_) { size_seen += x.size(); cap += x.capacity(); }
        struct_mib = cap * (sizeof(std::pair<const u64, float>) + 1.0) / 1048576.0;
        check([&](u64, const int* m) { u64 k = pack(m, d); return sh_[sid_of(k)].find(k)->second; });
#endif
#ifdef HAVE_BOOST
    } else if (mode == "boost-conc") {
        boost::concurrent_flat_map<u64, float> mp; mp.reserve(N);
        alloc_s = secs_since(t0); auto t1 = clk::now();
        par(N, [&](int, u64 lo, u64 hi) { walk_range(lo, hi, [&](u64, const int* m) { mp.emplace(pack(m, d), g(m)); }); });
        fill_s = secs_since(t1);
        size_seen = mp.size();
        check([&](u64, const int* m) { float v = -1; mp.visit(pack(m, d), [&](const auto& kv) { v = kv.second; }); return v; });
#endif
    } else if (mode == "oa-cas") {
        u64 cap = 1; while (double(N) / double(cap) > 0.875) cap <<= 1; const u64 mask = cap - 1;
        size_t bytes = cap * sizeof(CasSlot);
        CasSlot* sl = (CasSlot*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sl == MAP_FAILED) { perror("mmap"); return 1; }
        madvise(sl, bytes, MADV_HUGEPAGE);
        const u64 EMPTY = ~0ull;
        par(cap, [&](int, u64 lo, u64 hi) { for (u64 i = lo; i < hi; i++) sl[i].key.store(EMPTY, std::memory_order_relaxed); });
        alloc_s = secs_since(t0); auto t1 = clk::now();
        std::atomic<u64> dup{0};
        par(N, [&](int, u64 lo, u64 hi) { walk_range(lo, hi, [&](u64, const int* m) {
            u64 k = pack(m, d); float v = g(m); u64 i = mix64(k) & mask;
            for (;;) {
                u64 e = EMPTY;
                if (sl[i].key.compare_exchange_strong(e, k, std::memory_order_relaxed)) { sl[i].val = v; break; }
                if (e == k) { sl[i].val = v; dup++; break; }
                i = (i + 1) & mask;
            } }); });
        fill_s = secs_since(t1);                    // thread join orders the value stores
        size_seen = N - dup.load();
        struct_mib = bytes / 1048576.0;
        check([&](u64, const int* m) {
            u64 k = pack(m, d), i = mix64(k) & mask;
            while (sl[i].key.load(std::memory_order_relaxed) != k) i = (i + 1) & mask;
            return sl[i].val;
        });
        munmap(sl, bytes);
    } else {
        std::fprintf(stderr, "unknown or unavailable mode '%s'\n", mode.c_str()); return 1;
    }

    J("build_parallel").s("mode", mode).i("B", B).i("d", d).u("N", N).i("T", T_).u("seed", seed)
        .s("payload", payload).s("storage", storage).i("prefault", prefault).i("shards", S)
        .f("alloc_s", alloc_s).f("fill_s", fill_s).f("total_s", alloc_s + fill_s)
        .f("entries_per_s", N / (alloc_s + fill_s)).f("fill_entries_per_s", N / fill_s)
        .f("peak_rss_mib", peak_rss_mib()).f("struct_mib", struct_mib)
        .u("size_seen", size_seen).i("verified", V).u("errors", bad).u("content_hash", content_hash).s("thp", thp_mode()).emit();
    return bad || size_seen != N ? 3 : 0;
}
