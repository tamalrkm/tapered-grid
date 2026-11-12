// Listing 1 of the paper, compiled and run by `make verify`. Everything between
// the BEGIN and END markers is printed in the paper verbatim, so the listing
// cannot drift from the library.
#include "tg.hpp"
#include "pl.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

// The function being tabulated: the vector root of Section 7 of the paper.
static float vector_root(const int* m, int d, const double* s) { return pl::payload(m, d, s); }

int main() {
    double x[12] = {0, .01, .02, .04, .09, .16, .25, .36, .49, .64, .81, 1};
    tg::u64 r = 12345;
    // BEGIN LISTING
    tg::Shape   sh{16, 11};              // B = 16, 11 stored coordinates
    tg::Spacing sp = tg::Spacing::power(16, 2.0);       // s_j = ((j-1)/15)^2
    tg::Meta    meta;                    // recorded in the file header
    meta.pin_first = 1;                  // x[0] = 0 is not stored
    meta.spacing_kind = 1;  meta.spacing_param = 2.0;

    // Build once: 4 threads, each owning a range of indices; no locks.
    auto t = tg::Table<float>::create("root.tg", sh, meta);
    const double* s = sp.s.data();
    t.fill([&](const int* m, int d) { return vector_root(m, d, s); }, 4);
    t.sync();                            // flush, record the payload checksum

    // Serve: map read-only; header validated; MADV_RANDOM.
    auto memo = tg::Table<float>::open("root.tg");
    int m[11];
    tg::snap_nearest(x + 1, 11, sp, m);  // x sorted; x[0] = 0 is pinned
    float  p1 = memo[m];                 // one access
    double p2 = tg::interp_simplex(memo.data(), x + 1, 11, sp); // d+1 loads
    tg::unrank(r, 11, 16, m);            // the argument stored in slot r
    // END LISTING

    // Batched form of the same lookup, as the paper's Section 3.5 describes.
    int keys[2 * 11]; float out[2];
    tg::snap_nearest(x + 1, 11, sp, keys); tg::unrank(r, 11, 16, keys + 11);
    memo.get_many(keys, 2, out);
    assert(tg::rank(m, sh) == r);
    assert(out[0] == p1 && out[1] == memo.at(r));
    const double exact = pl::p1_newton(x, 12);
    std::printf("listing 1: f = %.6f, nearest %.6f, simplex %.6f; slot %llu round-trips\n",
                exact, (double)p1, p2, (unsigned long long)r);
    std::remove("root.tg");
    return 0;
}
