// End-to-end use: maximum-likelihood fit of a two-parameter Luce-type choice
// model, with the normalization p1 = f(x) computed exactly or through a memo.
//
// Model. Decision t offers l = 32 alternatives with sorted disadvantages
// 0 = delta_1 <= ... <= delta_l. With theta = (s, c):
//   e_i = (delta_i / s)^c,  x_i = 1 - exp(-e_i),  p1 = f(x),  p_i = p1^{exp(e_i)},
// so sum_i p_i = 1 by the definition of f. The log-likelihood of the chosen
// alternatives j_t is LL = sum_t exp(e_{j_t}) * ln p1_t. Data are synthetic:
// x^0 from the generator of Biswas & Regan (2015) Sec. 5, delta =
// s0 * (-ln(1 - x^0))^{1/c0}, and j_t drawn from the exact model at (s0, c0).
// Nelder-Mead on phi = (ln s, ln c), deterministic start and stopping rule.
//
// usage: pl_fit key=value ...
//   B=16 d=12 gamma=2   memo grid (d total coordinates, x_1 pinned)
//   n=20000 R=20        decisions per data set, replicate data sets
//   s0=1.0 c0=0.7       true parameters
//   threads=1           threads for the table build only (fits are single-threaded)
//   variants=exact,nearest,simplex,newton1,twostage[,hash]
//     nearest/simplex/hash: p1 from the memo (first d coordinates only)
//     newton1: memo value refined by one Newton iteration over all 32 coordinates
//     snewton1: the same, started from the simplex-interpolated value, which
//               keeps the objective continuous in the parameters
//     twostage: nearest fit, then an exact Nelder-Mead polish from its optimum
//   box=1               keep phi within ln s0 +- 4, ln c0 - 3 .. + 2 (outside: LL = -inf)
//   table=-             tg file to reuse/save; "-" = build in RAM
//   trace_out=FILE trace_B=22 trace_d=16 trace_max=100000
//                       write the exact fit's query stream of replicate 0,
//                       snapped to a (trace_B, trace_d) pinned grid, as
//                       {u64 rank; i64 lead} records for ooc_bench trace
#include "common.hpp"
#include "pl.hpp"
#include "tg.hpp"
#include <cmath>
#include <functional>
#include <map>
#include <sys/stat.h>
#if __has_include(<absl/container/flat_hash_map.h>)
#include <absl/container/flat_hash_map.h>
#define HAVE_ABSL 1
#endif

static const int L = 32;

struct Data { long n; std::vector<double> lndelta; std::vector<int> choice; };

static Data make_data(long n, double s0, double c0, unsigned seed) {
    Data D; D.n = n; D.lndelta.resize((size_t)n * L); D.choice.resize(n);
    std::vector<double> X0 = pl::generate_queries(n, L, 5.0, 12345u + 7919u * seed);
    u64 rs = 0x9E3779B97F4A7C15ull ^ (u64(seed) * 0xD1B54A32D192ED03ull); if (!rs) rs = 1;
    for (long t = 0; t < n; t++) {
        const double* x0 = &X0[(size_t)t * L];
        for (int i = 0; i < L; i++) {
            double delta = i == 0 ? 0.0 : s0 * std::pow(-std::log1p(-x0[i]), 1.0 / c0);
            D.lndelta[(size_t)t * L + i] = delta > 0 ? std::log(delta) : -INFINITY;
        }
        double p1 = pl::p1_newton(x0, L), u = (xs(rs) >> 11) * (1.0 / 9007199254740992.0), acc = 0;
        int j = L - 1;
        for (int i = 0; i < L; i++) { acc += std::pow(p1, 1.0 / std::max(1e-12, 1.0 - x0[i])); if (u < acc) { j = i; break; } }
        D.choice[t] = j;
    }
    return D;
}

// x_i for i < k at phi = (ln s, ln c), and e for the chosen alternative
static inline void transform(const double* lnd, int k, double lns, double c, double* x) {
    x[0] = 0.0;
    for (int i = 1; i < k; i++) { double e = std::exp(c * (lnd[i] - lns)); x[i] = -std::expm1(-e); }
}

struct Trace {
    FILE* f = nullptr; int B = 22, d = 16; long max = 100000, written = 0;
    tg::Spacing sp; std::vector<int> prev; long n = 0;
};

using P1 = std::function<double(const double*)>;   // p1 from x[0..L) (memo reads a prefix)

static double loglik(const Data& D, double lns, double lnc, int k, const P1& p1fn, long* evals, Trace* tr) {
    double c = std::exp(lnc), LL = 0, x[L];
    for (long t = 0; t < D.n; t++) {
        const double* lnd = &D.lndelta[(size_t)t * L];
        transform(lnd, k, lns, c, x);
        double p1 = p1fn(x);
        int j = D.choice[t];
        double ej = j == 0 ? 0.0 : std::exp(c * (lnd[j] - lns));
        LL += std::exp(ej) * std::log(p1);
        if (tr && tr->f && tr->written < tr->max) {
            int m[tg::MAX_D]; int dd = tr->d - 1;
            tg::snap_nearest(x + 1, dd, tr->sp, m);
            struct { u64 r; int64_t lead; } rec{tg::rank(m, dd, tr->B), -1};
            int* pv = &tr->prev[(size_t)t * dd];
            if (pv[0] == 0) rec.lead = 0;                // first visit of this decision: not a repeat
            else for (int i = 0; i < dd; i++) if (pv[i] != m[i]) { rec.lead = i; break; }
            std::memcpy(pv, m, dd * sizeof(int));
            std::fwrite(&rec, sizeof rec, 1, tr->f); tr->written++;
        }
    }
    if (evals) *evals += D.n;
    return LL;
}

struct Fit { double lns, lnc, LL; int iters; long evals; double secs; };

// Nelder-Mead maximizing LL over phi; fixed start, simplex, and stopping rule.
static Fit nelder_mead(std::function<double(double, double)> F, double a0, double b0, double step = 0.2) {
    double P[3][2] = {{a0, b0}, {a0 + step, b0}, {a0, b0 + step}}, V[3];
    for (int i = 0; i < 3; i++) V[i] = -F(P[i][0], P[i][1]);       // minimize -LL
    int it = 0;
    for (; it < 400; it++) {
        int o[3] = {0, 1, 2}; std::sort(o, o + 3, [&](int a, int b) { return V[a] < V[b]; });
        int lo = o[0], mi = o[1], hi = o[2];
        if (std::fabs(V[hi] - V[lo]) <= 1e-11 * (1 + std::fabs(V[lo]))) break;
        double cx = (P[lo][0] + P[mi][0]) / 2, cy = (P[lo][1] + P[mi][1]) / 2;
        auto at = [&](double t, double& x, double& y) { x = cx + t * (P[hi][0] - cx); y = cy + t * (P[hi][1] - cy); return -F(x, y); };
        double rx, ry, fr = at(-1, rx, ry);
        if (fr < V[lo]) { double ex, ey, fe = at(-2, ex, ey);
            if (fe < fr) { P[hi][0] = ex; P[hi][1] = ey; V[hi] = fe; } else { P[hi][0] = rx; P[hi][1] = ry; V[hi] = fr; } }
        else if (fr < V[mi]) { P[hi][0] = rx; P[hi][1] = ry; V[hi] = fr; }
        else { double kx, ky, fk = at(fr < V[hi] ? -0.5 : 0.5, kx, ky);
            if (fk < std::min(fr, V[hi])) { P[hi][0] = kx; P[hi][1] = ky; V[hi] = fk; }
            else for (int i : {mi, hi}) { P[i][0] = (P[i][0] + P[lo][0]) / 2; P[i][1] = (P[i][1] + P[lo][1]) / 2; V[i] = -F(P[i][0], P[i][1]); } }
    }
    int b = 0; for (int i = 1; i < 3; i++) if (V[i] < V[b]) b = i;
    return Fit{P[b][0], P[b][1], -V[b], it, 0, 0};
}

int main(int argc, char** argv) {
    std::map<std::string, std::string> a = {{"B", "16"}, {"d", "12"}, {"gamma", "2"}, {"n", "20000"}, {"R", "20"},
        {"s0", "1.0"}, {"c0", "0.7"}, {"threads", "1"}, {"variants", "exact,nearest,simplex"}, {"table", "-"},
        {"trace_out", ""}, {"trace_B", "22"}, {"trace_d", "16"}, {"trace_max", "100000"}, {"box", "1"}};
    for (int i = 1; i < argc; i++) { std::string s = argv[i]; auto e = s.find('=');
        if (e == std::string::npos || !a.count(s.substr(0, e))) { std::fprintf(stderr, "bad arg %s\n", argv[i]); return 1; }
        a[s.substr(0, e)] = s.substr(e + 1); }
    int B = std::stoi(a["B"]), d = std::stoi(a["d"]), R = std::stoi(a["R"]), threads = std::stoi(a["threads"]);
    long n = std::stol(a["n"]); double gamma = std::stod(a["gamma"]), s0 = std::stod(a["s0"]), c0 = std::stod(a["c0"]);
    std::string variants = "," + a["variants"] + ",";
    auto want = [&](const char* v) { return variants.find(std::string(",") + v + ",") != std::string::npos; };

    const int ds = d - 1; tg::Shape sh{B, ds}; tg::Spacing sp = tg::Spacing::power(B, gamma);
    const double* s = sp.s.data(); const u64 N = sh.size();

    // ---- memo table (built once, reused by every replicate and variant) ------
    tg::Table<float> tab; double build_s = 0, hash_insert_s = 0; bool reused = false;
    bool need_table = want("nearest") || want("simplex") || want("hash") || want("newton1") || want("snewton1") || want("twostage");
    if (need_table) {
        auto t0 = clk::now(); struct stat st;
        if (a["table"] != "-" && stat(a["table"].c_str(), &st) == 0) { tab = tg::Table<float>::open(a["table"]); reused = true; }
        else {
            tg::Meta meta; meta.pin_first = 1; meta.spacing_kind = 1; meta.spacing_param = gamma;
            tab = a["table"] == "-" ? tg::Table<float>::anonymous(sh, true) : tg::Table<float>::create(a["table"], sh, meta);
            tab.fill([&](const int* m, int dd) { return pl::payload(m, dd, s); }, threads);
            tab.sync(true);
        }
        build_s = secs_since(t0);
        if (tab.shape().B != B || tab.shape().d != ds) { std::fprintf(stderr, "table shape mismatch\n"); return 1; }
    }
    const float* arr = need_table ? tab.data() : nullptr;
#ifdef HAVE_ABSL
    absl::flat_hash_map<u64, float> hm; Packer pack(B, ds);
    if (want("hash")) { auto t0 = clk::now(); hm.reserve(N); int m[tg::MAX_D]; for (int i = 0; i < ds; i++) m[i] = 1;
        u64 r = 0; do hm.emplace(pack(m, ds), arr[r++]); while (tg::next(m, ds, B)); hash_insert_s = secs_since(t0); }
#else
    if (want("hash")) { std::fprintf(stderr, "variant hash needs absl\n"); return 1; }
#endif
    J("pl_fit").s("what", "table").i("B", B).i("d", d).f("gamma", gamma).u("N", N).f("table_mib", N * 4.0 / 1048576.0)
        .i("threads", threads).i("reused", reused).f("build_s", build_s).f("hash_insert_s", hash_insert_s).emit();

    std::map<std::string, P1> fns;
    fns["exact"] = [](const double* x) { return pl::p1_newton(x, L); };
    fns["nearest"] = [&](const double* x) { int m[tg::MAX_D]; tg::snap_nearest(x + 1, ds, sp, m); return (double)arr[tg::rank(m, ds, B)]; };
    fns["simplex"] = [&](const double* x) { return tg::interp_simplex(arr, x + 1, ds, sp); };
    fns["newton1"] = [&](const double* x) { return pl::p1_newton_from(x, L, fns["nearest"](x), 1); };
    fns["snewton1"] = [&](const double* x) { return pl::p1_newton_from(x, L, fns["simplex"](x), 1); };
#ifdef HAVE_ABSL
    fns["hash"] = [&](const double* x) { int m[tg::MAX_D]; tg::snap_nearest(x + 1, ds, sp, m); return (double)hm.find(pack(m, ds))->second; };
#endif

    Trace tr;
    if (!a["trace_out"].empty()) {
        tr.f = std::fopen(a["trace_out"].c_str(), "wb"); tr.B = std::stoi(a["trace_B"]); tr.d = std::stoi(a["trace_d"]);
        tr.max = std::stol(a["trace_max"]); tr.sp = tg::Spacing::power(tr.B, gamma);
        tr.prev.assign((size_t)n * (tr.d - 1), 0);
    }

    const double a0 = std::log(s0) + 0.5, b0 = std::log(c0) - 0.3;     // fixed, deliberately off start
    for (int rep = 0; rep < R; rep++) {
        Data D = make_data(n, s0, c0, rep + 1);
        Fit ex{}; double se_s = NAN, se_c = NAN;           // exact optimum and its standard errors
        const bool box = a["box"] == "1";
        auto inside = [&](double x, double y) { return !box || (std::fabs(x - std::log(s0)) <= 4 && y >= std::log(c0) - 3 && y <= std::log(c0) + 2); };
        for (const char* v : {"exact", "nearest", "simplex", "newton1", "snewton1", "twostage", "hash"}) {
            std::string vs = v;
            if (!want(v) || (vs != "twostage" && !fns.count(v))) continue;
            // memo variants read only the first d coordinates; exact and newton1 need all 32
            auto objective = [&](const std::string& fn, long* evals, Trace* trp) {
                int k = (fn == "exact" || fn == "newton1" || fn == "snewton1") ? L : d;
                return [&, fn, k, evals, trp](double x, double y) {
                    return inside(x, y) ? loglik(D, x, y, k, fns[fn], evals, trp) : -1e300; };
            };
            long evals = 0, evals_exact = 0; Trace* trp = (rep == 0 && vs == "exact") ? &tr : nullptr;
            auto t0 = clk::now(); Fit f;
            if (vs == "twostage") {
                Fit f1 = nelder_mead(objective("nearest", &evals, nullptr), a0, b0);
                f = nelder_mead(objective("exact", &evals_exact, nullptr), f1.lns, f1.lnc, 0.05);
                f.iters += f1.iters; evals += evals_exact;
            } else f = nelder_mead(objective(vs, &evals, trp), a0, b0);
            f.secs = secs_since(t0); f.evals = evals;
            J j("pl_fit"); j.s("what", "fit").s("variant", v).i("rep", rep).i("B", B).i("d", d).i("n", n)
                .f("lns", f.lns).f("lnc", f.lnc).f("LL", f.LL).i("iters", f.iters).i("f_evals", f.evals)
                .f("fit_s", f.secs).f("ns_per_eval", f.secs * 1e9 / f.evals).i("exact_f_evals", vs == "exact" ? evals : evals_exact);
            if (vs == "exact") {
                ex = f;
                // observed information at the exact optimum: central differences, h = 1e-3
                const double h = 1e-3; auto Lx = [&](double x, double y) { return loglik(D, x, y, L, fns["exact"], nullptr, nullptr); };
                double H11 = (Lx(f.lns + h, f.lnc) - 2 * f.LL + Lx(f.lns - h, f.lnc)) / (h * h);
                double H22 = (Lx(f.lns, f.lnc + h) - 2 * f.LL + Lx(f.lns, f.lnc - h)) / (h * h);
                double H12 = (Lx(f.lns + h, f.lnc + h) - Lx(f.lns + h, f.lnc - h) - Lx(f.lns - h, f.lnc + h) + Lx(f.lns - h, f.lnc - h)) / (4 * h * h);
                double det = H11 * H22 - H12 * H12;           // cov = (-H)^-1
                se_s = std::sqrt(std::max(0.0, -H22 / det)); se_c = std::sqrt(std::max(0.0, -H11 / det));
                j.f("se_lns", se_s).f("se_lnc", se_c)
                 .f("z_true_lns", (f.lns - std::log(s0)) / se_s).f("z_true_lnc", (f.lnc - std::log(c0)) / se_c);
            } else if (want("exact")) {
                j.f("dz_lns", (f.lns - ex.lns) / se_s).f("dz_lnc", (f.lnc - ex.lnc) / se_c)
                 .f("dLL_exact", loglik(D, f.lns, f.lnc, L, fns["exact"], nullptr, nullptr) - ex.LL);
            }
            j.emit();
        }
    }
    if (tr.f) { std::fclose(tr.f); J("pl_fit").s("what", "trace").s("file", a["trace_out"]).i("records", tr.written)
        .i("B", tr.B).i("d", tr.d).emit(); }
    return 0;
}
