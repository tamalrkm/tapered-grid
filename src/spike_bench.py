"""
GO/NO-GO benchmark spike for the re-angled (experimental-algorithms) paper.

Question the whole paper hinges on:
  As a MEMOIZATION structure for a sorted-argument symmetric function, does the
  tapered flat-array + O(d) closed-form address actually beat (a) a hash memo
  table and (b) a kd-tree, on memory / query latency / accuracy?

f = the paper's log-case p1: given a non-increasing a in (0,1], the top-choice
probability p1 solving  sum_i p1^{1/a_i} = 1  (Plackett-Luce / Luce). Exact eval
is an iterative root-find (the "expensive" part we memoize). Grid lives in
x = 1 - a, sorted non-decreasing, quantized to B core branches.
"""

import sys, time, array
import numpy as np
from itertools import combinations_with_replacement
from scipy.spatial import cKDTree
from refimpl import rank_fixed, unrank_fixed

# ---- the expensive exact function -----------------------------------------
def p1_exact(a):
    """Solve sum_i a? ... p1^{1/a_i} = 1 for p1 in (0,1) by bisection (1e-13)."""
    inv = [1.0 / ai for ai in a]
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        s = 0.0
        for e in inv:
            s += mid ** e
        if s < 1.0:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)

# ---- grid ------------------------------------------------------------------
B, D = 16, 6
BRANCH = np.linspace(0.0, 0.9375, B)          # x core-branch values (a>=0.0625)

def cols_to_a(cols):
    # column indices (1..B) -> x -> a = 1 - x, kept non-increasing in a
    return [1.0 - BRANCH[c - 1] for c in cols]

def build():
    combos = list(combinations_with_replacement(range(1, B + 1), D))
    N = len(combos)
    vals = np.empty(N, dtype=np.float32)       # tapered store: VALUES ONLY
    hashmemo = {}                               # tuple(cols) -> value
    coords = np.empty((N, D), dtype=np.float32) # kd-tree needs the points
    for combo in combos:
        r = rank_fixed(list(combo), B)          # O(d) address
        v = np.float32(p1_exact(cols_to_a(combo)))
        vals[r] = v
        hashmemo[combo] = v
        coords[r] = [BRANCH[c - 1] for c in combo]
    tree = cKDTree(coords)
    return N, combos, vals, hashmemo, coords, tree

# ---- interpolate an arbitrary sorted x onto the grid (nearest col, monotone)
def snap(x):
    cols, lo = [], 1
    for xi in x:
        c = lo
        while c < B and abs(BRANCH[c] - xi) < abs(BRANCH[c - 1] - xi):
            c += 1
        cols.append(c); lo = c
    return cols

def rand_query(rng):
    # b-style generation from the paper (monotone, tapered toward 1)
    p, x, b = 0.0, [], 5.0
    for _ in range(D):
        r = rng.uniform(p, 0.9375)
        p = p + (r - p) / b
        x.append(p)
    return sorted(x)

def main():
    t0 = time.perf_counter()
    N, combos, vals, hashmemo, coords, tree = build()
    t_build = time.perf_counter() - t0

    # correctness: the O(d) address must agree with the hash memo on EVERY point
    bad = sum(1 for c in combos if vals[rank_fixed(list(c), B)] != hashmemo[c])
    assert bad == 0, bad

    rng = np.random.default_rng(0)
    Q = 5000
    queries = [rand_query(rng) for _ in range(Q)]
    snapped = [snap(x) for x in queries]

    # --- latency ---
    t = time.perf_counter()
    for cols in snapped:
        _ = vals[rank_fixed(cols, B)]
    lat_tap = (time.perf_counter() - t) / Q * 1e6
    t = time.perf_counter()
    for cols in snapped:
        _ = hashmemo[tuple(cols)]
    lat_hash = (time.perf_counter() - t) / Q * 1e6
    t = time.perf_counter()
    qpts = np.array([[BRANCH[c - 1] for c in cols] for cols in snapped], dtype=np.float32)
    _d, _i = tree.query(qpts)
    lat_kd = (time.perf_counter() - t) / Q * 1e6

    # --- accuracy vs exact (all three snap to the same grid, so ~equal) ---
    err_tap = np.array([abs(float(vals[rank_fixed(cols, B)]) - p1_exact(cols_to_a(snap(x))))
                        for x, cols in zip(queries, snapped)])

    # --- memory: packed byte model (implementation-independent) ---
    def model(N, d):
        return {
            "tapered flat array (values only, args reconstructable)": 4 * N,
            "hash memo (values + 1B/col keys)":                       4 * N + d * N,
            "kd-tree (values + 4B/coord + ~tree)":                    4 * N + 4 * d * N,
        }

    print(f"grid: B={B} d={D}  N={N:,}   build={t_build:.2f}s   addr-correctness: OK ({bad} mismatches)")
    print(f"\nquery latency (us/query, pure-Python micro-bench):")
    print(f"  tapered  O(d) rank + array : {lat_tap:7.2f}")
    print(f"  hash     dict lookup       : {lat_hash:7.2f}")
    print(f"  kd-tree  NN query (batched): {lat_kd:7.2f}")
    print(f"\naccuracy vs exact p1 (all snap to same grid):")
    print(f"  mean|err|={err_tap.mean():.2e}  max|err|={err_tap.max():.2e}  bias={err_tap.mean():.2e}")
    print(f"\nmemory (packed byte model):")
    for scale, (n, d) in {"this spike": (N, D), "paper's file": (77_558_760, 14)}.items():
        print(f"  @ {scale} (N={n:,}, d={d}):")
        m = model(n, d)
        base = m["tapered flat array (values only, args reconstructable)"]
        for k, v in m.items():
            print(f"     {k:52s} {v/1e6:9.1f} MB   ({v/base:.2f}x)")

if __name__ == "__main__":
    main()
