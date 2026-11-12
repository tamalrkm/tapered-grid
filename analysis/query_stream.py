#!/usr/bin/env python3
"""Working set of the lookup benchmarks' query stream, by grid.

A benchmark key is d independent uniform column indices in 1..B, sorted (common.hpp,
gen_query), so keys are not uniform over the grid. This script draws 3M such keys per
grid of the window plan (numpy, fixed seed: the same distribution, not the identical
stream) and reports how many distinct keys and array cache lines they touch, and which
share of queries the hottest 32 MiB would serve (one server L3 slice) for the array
(16 floats per 64-byte line, lexicographic order) and for a hash table (one line per key).
Writes results/query_stream.json. Needs numpy.
"""
import json, math, os, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
Q, HOT = 3_000_000, 32 * 2**20 // 64
GRIDS = [(48, 6), (28, 8), (20, 10), (16, 13), (14, 15), (13, 16), (11, 20)]


def main():
    rng = np.random.default_rng(20261112)
    out = []
    for B, d in GRIDS:
        N = math.comb(B + d - 1, d)
        C = np.array([[math.comb(n, k) if k <= n else 0 for k in range(d + 2)] for n in range(B + d + 2)], dtype=np.int64)
        m = np.sort(rng.integers(1, B + 1, size=(Q, d), dtype=np.int64), axis=1)
        s = np.zeros(Q, dtype=np.int64)
        for i in range(1, d + 1):
            s += C[B - m[:, i - 1] + d - i, d - i + 1]
        r = N - 1 - s                                     # Eq. (1)
        _, kc = np.unique(r, return_counts=True)
        _, lc = np.unique(r // 16, return_counts=True)
        out.append({"B": B, "d": d, "N": N, "queries": Q, "distinct_keys": int(kc.size), "array_lines": int(lc.size),
                    "uniform_lines": round((N / 16) * (1 - math.exp(-Q / (N / 16)))),
                    "hot32_array": float(np.sort(lc)[::-1][:HOT].sum() / Q),
                    "hot32_hash": float(np.sort(kc)[::-1][:HOT].sum() / Q)})
    json.dump(out, open(os.path.join(ROOT, "results", "query_stream.json"), "w"), indent=1)
    for g in out:
        print(g)


if __name__ == "__main__":
    sys.exit(main())
