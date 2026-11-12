// pl.hpp -- the vector-root function f of Biswas and Regan (2015), used by pl_bench,
// pl_fit and build_parallel (payload "pl"; "pl" is a historical name: the function
// is the logarithmic case of their model, not Luce's rule, which has a closed form).
//
// f(x) = p1 solving  sum_i p1^{1/(1-x_i)} = 1,  x_1 = 0, x non-decreasing in
// [0,1). No closed form: each exact evaluation is an iterative root-find whose
// every iteration costs one pow() per coordinate.
//
// Floating-point policy. p1_newton() is evaluated in double and stored as
// float by round-to-nearest (static_cast). It is marked noinline and every
// binary is compiled with -ffp-contract=off and without -ffast-math, so each
// call site runs the same machine code: a table built from it and a hash memo
// built from it hold bit-identical payloads. Rebuilding on a machine with a
// different libm pow() may change the last bit; ship the table file (its header
// carries a payload checksum) rather than the recipe.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pl {

// Exact f: Newton on sum p^{b_i} = 1, b_i = 1/(1 - x_i).
__attribute__((noinline)) inline double p1_newton(const double* x, int n) {
    double p = 0.5;
    for (int it = 0; it < 40; it++) {
        double s = 0, ds = 0;
        for (int i = 0; i < n; i++) {
            double b = 1.0 / std::max(1e-12, 1.0 - x[i]);
            double t = std::pow(p, b); s += t; ds += b * t / p;
        }
        double step = (s - 1.0) / ds; p -= step;
        if (p <= 1e-12) p = 1e-12;
        if (p >= 1) p = 1 - 1e-12;
        if (std::fabs(step) < 1e-13) break;
    }
    return p;
}

// k Newton iterations over the full vector started from p0 (e.g. a memo
// value): the "memo as initial guess" path.
inline double p1_newton_from(const double* x, int n, double p, int k) {
    for (int it = 0; it < k; it++) {
        double s = 0, ds = 0;
        for (int i = 0; i < n; i++) {
            double b = 1.0 / std::max(1e-12, 1.0 - x[i]);
            double t = std::pow(p, b); s += t; ds += b * t / p;
        }
        p -= (s - 1.0) / ds;
        if (p <= 1e-12) p = 1e-12;
        if (p >= 1) p = 1 - 1e-12;
    }
    return p;
}

__attribute__((noinline)) inline double p1_bisect(const double* x, int n) {
    double lo = 0, hi = 1;
    for (int it = 0; it < 50; it++) {
        double m = 0.5 * (lo + hi), s = 0;
        for (int i = 0; i < n; i++) s += std::pow(m, 1.0 / std::max(1e-12, 1.0 - x[i]));
        if (s < 1.0) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

// Query generator of Biswas & Regan (2015), Sec. 5: x_1 = 0; repeatedly
// r ~ U(p, 1), x_next = p + (r - p)/b, p = x_next. A fixed LCG, so the query set
// is the same on every machine.
inline std::vector<double> generate_queries(long n, int K, double b, unsigned seed = 12345) {
    std::vector<double> X((size_t)n * K);
    unsigned st = seed;
    for (long q = 0; q < n; q++) {
        double p = 0; X[(size_t)q * K] = 0;
        for (int i = 1; i < K; i++) {
            st = st * 1103515245u + 12345u;
            double r = p + ((st >> 16) / 65535.0) * (1.0 - p);
            p = p + (r - p) / b;
            X[(size_t)q * K + i] = std::min(p, 0.999999);
        }
    }
    return X;
}

// Payload for a stored key of a pinned grid: the stored columns m[0..d-1] are
// coordinates 2..d+1; coordinate 1 is x_1 = 0.
inline float payload(const int* m, int d, const double* s) {
    double x[80]; x[0] = 0.0;
    for (int i = 0; i < d; i++) x[i + 1] = s[m[i]];
    return static_cast<float>(p1_newton(x, d + 1));
}

} // namespace pl
