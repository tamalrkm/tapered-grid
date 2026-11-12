// Compiled by analysis/loopstats.py only: the two forms of the rank as separate
// functions, so that the instructions and loads of their loops can be counted
// from the disassembly with the flags of the benchmarks.
#include "tg.hpp"
using tg::u64;
__attribute__((noinline)) u64 rank_one_read(const int* m, int d, int B, const tg::Pascal& P) { return tg::rank(m, d, B, P); }
__attribute__((noinline)) u64 rank_two_read(const int* m, int d, int B, const tg::Pascal& P) {
    int prev = 1; u64 r = 0;
    for (int i = 1; i <= d; i++) {
        int mi = m[i-1], k = d - i;
        r += P(B - prev + k + 1, k + 1) - P(B - mi + k + 1, k + 1);
        prev = mi;
    }
    return r;
}
int main() { return 0; }
