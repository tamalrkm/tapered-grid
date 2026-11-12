// Vector-root memoization: single-query microbenchmark, accuracy of the two
// lookup paths, and (optionally) bit-identity against a hash memo.
//
// usage: pl_bench B d gamma [b=5] [threads=1] [table=-] [verify_hash=0] [reps=1]
//   d      total coordinates; x_1 = 0 is pinned, so the table stores d-1
//   table  path of a tg file to reuse (built and saved if absent); "-" = RAM
//   verify_hash=1 (needs absl): every key's payload is recomputed independently
//          into an absl::flat_hash_map keyed by the packed key, and compared
//          bitwise with the array slot at rank(key); then every query's
//          nearest-gridpoint answer is compared bitwise through both memos.
//   reps   repeat the latency loops (fresh timing each, same process)
//
// Lookup paths: nearest (snap + rank + one read), simplex (d+1 reads on the
// Kuhn simplex), nearest+newton1 (memo value as the start of one Newton
// iteration over the full vector). Accuracy is reported against f on the full
// vector and against f on the first d coordinates (truncation removed).
//
// Queries: 2e5 vectors of length 32 from the generator of Biswas & Regan
// (2015) Sec. 5; the memo uses the first d coordinates.
#include "common.hpp"
#include "pl.hpp"
#include "tg.hpp"
#include <mutex>
#include <thread>
#include <sys/stat.h>
#if __has_include(<absl/container/flat_hash_map.h>)
#include <absl/container/flat_hash_map.h>
#define HAVE_ABSL 1
#endif

static const int K = 32;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s B d gamma [b] [threads] [table] [verify_hash] [reps]\n", argv[0]); return 1; }
    int B = atoi(argv[1]), d = atoi(argv[2]); double gamma = atof(argv[3]);
    double bp = argc > 4 ? atof(argv[4]) : 5.0;
    int threads = argc > 5 ? atoi(argv[5]) : 1;
    std::string tpath = argc > 6 ? argv[6] : "-";
    bool verify_hash = argc > 7 && atoi(argv[7]);
    int reps = argc > 8 ? atoi(argv[8]) : 1;

    const int ds = d - 1;                               // stored coordinates
    tg::Shape sh{B, ds};
    tg::Spacing sp = tg::Spacing::power(B, gamma);
    const double* s = sp.s.data();
    const u64 N = sh.size();
    tg::Meta meta; meta.pin_first = 1; meta.spacing_kind = 1; meta.spacing_param = gamma;
    std::snprintf(meta.note, sizeof meta.note, "pl::p1_newton, gcc %d.%d, -ffp-contract=off", __GNUC__, __GNUC_MINOR__);

    const long NQ = 200000;
    std::vector<double> X = pl::generate_queries(NQ, K, bp);

    // ---- table: reuse a saved file or build ----------------------------------
    tg::Table<float> tab; double build_s = 0; bool reused = false;
    auto t0 = clk::now();
    struct stat st;
    if (tpath != "-" && stat(tpath.c_str(), &st) == 0) {
        tab = tg::Table<float>::open(tpath, tg::Access::random, true);
        if (tab.shape().B != B || tab.shape().d != ds || tab.meta().spacing_param != gamma || !tab.meta().pin_first) {
            std::fprintf(stderr, "table file does not match B/d/gamma\n"); return 1; }
        reused = true;
    } else {
        tab = tpath == "-" ? tg::Table<float>::anonymous(sh, true) : tg::Table<float>::create(tpath, sh, meta);
        tab.fill([&](const int* m, int dd) { return pl::payload(m, dd, s); }, threads);
        tab.sync(true);
    }
    build_s = secs_since(t0);
    const float* arr = tab.data();
    J("pl_bench").s("what", "build").i("B", B).i("d", d).f("gamma", gamma).u("N", N)
        .f("table_mib", N * 4.0 / 1048576.0).i("threads", threads).i("reused", reused).f("build_s", build_s).emit();

    // ---- accuracy of both lookup paths ---------------------------------------
    // exact: f on the full 32-vector. trunc: f on the first d coordinates only,
    // which is what any table over d coordinates can at best return, so
    // memo - exact = (memo - trunc) [interpolation] + (trunc - exact) [truncation].
    std::vector<double> exact(NQ), trunc(NQ);
    for (long q = 0; q < NQ; q++) { exact[q] = pl::p1_newton(&X[(size_t)q * K], K); trunc[q] = pl::p1_newton(&X[(size_t)q * K], d); }
    auto nearest = [&](const double* x) { int m[tg::MAX_D]; tg::snap_nearest(x + 1, ds, sp, m); return arr[tg::rank(m, ds, B)]; };
    auto simplex = [&](const double* x) { return tg::interp_simplex(arr, x + 1, ds, sp); };
    auto newton1 = [&](const double* x) { return pl::p1_newton_from(x, K, nearest(x), 1); };
    auto accuracy = [&](const char* path, auto look) {
        std::vector<double> e(NQ); double bias = 0, mean = 0, mean_interp = 0, bias_interp = 0;
        for (long q = 0; q < NQ; q++) { double v = look(&X[(size_t)q * K]);
            bias += v - exact[q]; e[q] = std::fabs(v - exact[q]); mean += e[q];
            bias_interp += v - trunc[q]; mean_interp += std::fabs(v - trunc[q]); }
        bias /= NQ; mean /= NQ; bias_interp /= NQ; mean_interp /= NQ; std::sort(e.begin(), e.end());
        J("pl_bench").s("what", "accuracy").s("path", path).i("B", B).i("d", d).u("N", N)
            .f("err_mean", mean).f("err_p50", e[NQ / 2]).f("err_p99", e[size_t(0.99 * (NQ - 1))]).f("err_max", e[NQ - 1])
            .f("bias", bias).f("bias_over_mean", std::fabs(bias) / mean)
            .f("vs_trunc_err_mean", mean_interp).f("vs_trunc_bias", bias_interp).emit();
    };
    accuracy("trunc", [&](const double* x) { return pl::p1_newton(x, d); });
    accuracy("nearest", nearest);
    accuracy("simplex", simplex);
    accuracy("nearest+newton1", newton1);

    // ---- single-query latency ------------------------------------------------
    const long ML = 100000; volatile double sink = 0;
    for (int rep = 0; rep < reps; rep++) {
        auto time = [&](auto fn) { auto t = clk::now(); for (long q = 0; q < ML; q++) sink = sink + fn(&X[(size_t)q * K]); return secs_since(t) * 1e9 / ML; };
        double ns_near = time(nearest), ns_simp = time(simplex), ns_n1 = time(newton1);
        double ns_newton = time([](const double* x) { return pl::p1_newton(x, K); });
        double ns_bis = time([](const double* x) { return pl::p1_bisect(x, K); });
        J("pl_bench").s("what", "latency").i("rep", rep).i("B", B).i("d", d).u("N", N)
            .f("nearest_ns", ns_near).f("simplex_ns", ns_simp).f("newton1_ns", ns_n1).f("newton_ns", ns_newton).f("bisect_ns", ns_bis)
            .f("speedup_nearest_newton", ns_newton / ns_near).f("speedup_simplex_newton", ns_newton / ns_simp).emit();
    }

    // ---- bit identity with an independently built hash memo -------------------
    if (verify_hash) {
#ifdef HAVE_ABSL
        Packer pack(B, ds);
        const int SB = 10, S = 1 << SB;
        std::vector<absl::flat_hash_map<u64, float>> sh_(S); std::vector<std::mutex> mu(S);
        for (auto& x : sh_) x.reserve(N / S + N / S / 8 + 16);
        auto sid = [&](u64 k) { return int(mix64(k) >> (64 - SB)); };
        std::vector<std::thread> ws; std::vector<u64> bad(threads, 0), seen(threads, 0);
        auto t1 = clk::now();
        // phase 1: each worker enumerates its own key range, evaluates f afresh, inserts
        for (int j = 0; j < threads; j++) ws.emplace_back([&, j] {
            u64 lo = (unsigned __int128)N * j / threads, hi = (unsigned __int128)N * (j + 1) / threads;
            if (lo >= hi) return;
            int m[tg::MAX_D]; tg::unrank(lo, ds, B, m);
            for (u64 r = lo; r < hi; r++) { u64 k = pack(m, ds); float v = pl::payload(m, ds, s);
                { std::lock_guard<std::mutex> lk(mu[sid(k)]); sh_[sid(k)].emplace(k, v); } tg::next(m, ds, B); } });
        for (auto& w : ws) w.join(); ws.clear();
        // phase 2: every key, hash value vs array[rank(key)] bitwise
        for (int j = 0; j < threads; j++) ws.emplace_back([&, j] {
            u64 lo = (unsigned __int128)N * j / threads, hi = (unsigned __int128)N * (j + 1) / threads;
            if (lo >= hi) return;
            int m[tg::MAX_D]; tg::unrank(lo, ds, B, m);
            for (u64 r = lo; r < hi; r++) { u64 k = pack(m, ds); auto it = sh_[sid(k)].find(k);
                float a = arr[tg::rank(m, ds, B)];
                if (it == sh_[sid(k)].end() || std::memcmp(&a, &it->second, 4) != 0 || tg::rank(m, ds, B) != r) bad[j]++;
                seen[j]++; tg::next(m, ds, B); } });
        for (auto& w : ws) w.join();
        u64 nbad = 0, nseen = 0, nsize = 0; for (int j = 0; j < threads; j++) { nbad += bad[j]; nseen += seen[j]; }
        for (auto& x : sh_) nsize += x.size();
        // phase 3: every query through both memos
        u64 qbad = 0;
        for (long q = 0; q < NQ; q++) { int m[tg::MAX_D]; tg::snap_nearest(&X[(size_t)q * K] + 1, ds, sp, m);
            float a = arr[tg::rank(m, ds, B)], h = sh_[sid(pack(m, ds))].find(pack(m, ds))->second;
            if (std::memcmp(&a, &h, 4) != 0) qbad++; }
        J("pl_bench").s("what", "verify_hash").i("B", B).i("d", d).u("N", N).u("keys_checked", nseen)
            .u("hash_size", nsize).u("key_mismatches", nbad).i("queries_checked", NQ).u("query_mismatches", qbad)
            .f("verify_s", secs_since(t1)).emit();
        if (nbad || qbad || nsize != N) return 2;
#else
        std::fprintf(stderr, "verify_hash needs absl\n"); return 1;
#endif
    }
    return 0;
}
