#!/usr/bin/env python3
"""The taper guide of Section 4 applied to the vector root: measured decay of influence.

f(x) = p solving sum_i p^{b_i} = 1, b_i = 1/(1 - x_i). Implicit differentiation gives
df/dx_i = -p^{b_i} ln(p) b_i^2 / sum_j b_j p^{b_j - 1}, so the sensitivities need no
finite differences. Queries come from the generator of Biswas and Regan (b = 5, l = 32),
as in pl.hpp. Writes one JSON object to stdout. Needs NumPy.
"""
import json, sys
import numpy as np


def queries(n, K=32, b=5.0, seed=12345):
    """pl::generate_queries: the same LCG, so the same queries as the benchmark."""
    st = seed
    X = np.zeros((n, K))
    for q in range(n):
        p = 0.0
        for i in range(1, K):
            st = (st * 1103515245 + 12345) & 0xFFFFFFFF
            r = p + ((st >> 16) / 65535.0) * (1.0 - p)
            p = p + (r - p) / b
            X[q, i] = min(p, 0.999999)
    return X


def f(X):
    b = 1.0 / np.maximum(1e-12, 1.0 - X)
    p = np.full(X.shape[0], 0.5)
    for _ in range(60):
        s = (p[:, None] ** b).sum(1) - 1
        ds = (b * p[:, None] ** (b - 1)).sum(1)
        p = np.clip(p - s / ds, 1e-12, 1 - 1e-12)
    return p, b


def main():
    X = queries(20000)
    p, b = f(X)
    ds = (b * p[:, None] ** (b - 1)).sum(1)
    g = np.abs(p[:, None] ** b * np.log(p)[:, None] * b ** 2 / ds[:, None]).mean(0)
    i = np.arange(2, 17)                         # coordinates 2..16; x_1 is pinned
    slope, lnG = np.polyfit(i - 1, np.log(g[1:16]), 1)
    print(json.dumps({"r": float(np.exp(slope)), "G": float(np.exp(lnG)),
                      "g": [float(v) for v in g[:32]], "queries": int(X.shape[0])}))


if __name__ == "__main__":
    sys.exit(main())
