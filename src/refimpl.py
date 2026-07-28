"""
Table-free ranking / unranking for the tapered-grid data structure of
Biswas & Regan, TCS 607 (2015) 113-123.

Core claim being verified here
------------------------------
The grid's index computation (Eq. 3 of the paper), which the paper realizes
with an O(B*d) table V[i,b] plus a second O(B*d) prefix-sum table, is exactly
the LEXICOGRAPHIC RANK of a combination-with-repetition (a non-decreasing
sequence of column indices).  Recognizing this lets the per-level column sum
collapse via the hockey-stick identity to a *difference of two binomials*,
giving a closed form that needs NO scheme-specific table -- only binomials.

Closed form (0-based lexicographic rank), for a non-decreasing column vector
m = (m_1,...,m_d), each m_i in [1,B], with the sentinel m_0 = 1:

    rank = sum_{i=1}^{d} [ C(B - m_{i-1} + (d-i) + 1, d-i+1)
                         - C(B - m_i     + (d-i) + 1, d-i+1) ]

Derivation of the hockey-stick collapse.  A sequence m' is lexicographically
smaller than m iff its first point of difference is some position i, with
m'_1..m'_{i-1} = m_1..m_{i-1} and m'_i in [m_{i-1}, m_i - 1] (monotonicity
forces m'_i >= m_{i-1}).  The remaining d-i positions are free and >= m'_i, so
there are C(B - m'_i + (d-i), d-i) completions.  Summing that over
m'_i = m_{i-1} .. m_i - 1 is a run of consecutive binomials along one diagonal;
by  sum_{t=lo}^{hi} C(t,k) = C(hi+1,k+1) - C(lo,k+1)  it telescopes to the two
binomials above.  Total work is O(d) with a factorial table, and O(0) tables
built per grid scheme.
"""

from math import comb
from itertools import combinations_with_replacement


# --------------------------------------------------------------------------
# 1. Fixed-branching grid: closed-form rank (table-free)
# --------------------------------------------------------------------------
def rank_fixed(cols, B):
    """0-based lexicographic rank of a non-decreasing column vector `cols`
    (values in 1..B) among all such length-d vectors.  O(d), no table."""
    d = len(cols)
    m_prev = 1          # sentinel m_0 = minimum column
    r = 0
    for i in range(1, d + 1):
        mi = cols[i - 1]
        k = d - i                       # free positions after i
        r += comb(B - m_prev + k + 1, k + 1) - comb(B - mi + k + 1, k + 1)
        m_prev = mi
    return r


def unrank_fixed(r, B, d):
    """Inverse of rank_fixed: recover the column vector from a 0-based index.
    Greedy 'combinadic' decode, O(d * B) worst case, O(d log) with search.
    The paper provides no inverse; this comes for free from the same view."""
    cols = []
    lo = 1
    for i in range(1, d + 1):
        L = d - i                       # free positions after this one
        v = lo
        while True:
            block = comb(B - v + L, L)  # completions with position i == v
            if r < block:
                cols.append(v)
                lo = v
                break
            r -= block
            v += 1
    return cols


# --------------------------------------------------------------------------
# 2. Ground truth: brute-force enumeration in lexicographic order
# --------------------------------------------------------------------------
def rank_bruteforce(cols, B):
    """Reference rank by explicit enumeration. combinations_with_replacement
    yields non-decreasing tuples in ascending lexicographic order -- exactly
    the paper's ordering (index 0 = all-minimum, last = (min, max))."""
    d = len(cols)
    target = tuple(cols)
    for idx, t in enumerate(combinations_with_replacement(range(1, B + 1), d)):
        if t == target:
            return idx
    raise ValueError("target not found")


def total_count(B, d):
    """|U| for a fixed-branch length-d grid = C(B + d - 1, d) multiset coeff."""
    return comb(B + d - 1, d)


# --------------------------------------------------------------------------
# 3. Tests
# --------------------------------------------------------------------------
def test_exhaustive():
    """For small (B,d), verify closed form == brute force for EVERY vector,
    and that unrank is a true inverse of rank."""
    checked = 0
    for B in range(1, 9):
        for d in range(1, 7):
            for idx, t in enumerate(
                    combinations_with_replacement(range(1, B + 1), d)):
                cols = list(t)
                rf = rank_fixed(cols, B)
                assert rf == idx, (B, d, cols, rf, idx)
                assert unrank_fixed(rf, B, d) == cols, (B, d, cols)
                checked += 1
            # last index must be total_count - 1
            assert idx == total_count(B, d) - 1
    return checked


def test_paper_example1():
    """Paper Example 1 (Sec 3.2, Table 5): B=17, d=5,
    u = (0.00,0.00,0.02,0.04,0.80) -> columns (1,1,3,5,15) -> index 328."""
    cols = [1, 1, 3, 5, 15]
    r = rank_fixed(cols, 17)
    assert r == 328, r
    # brute-force cross-check on the same instance
    assert rank_bruteforce(cols, 17) == 328
    # round trip
    assert unrank_fixed(328, 17, 5) == cols
    return r


def test_experiment_size():
    """Paper Sec 5: fixed grid B=16, effective free depth (a_1 fixed) gives the
    reported file size 77,558,760.  Report what the multiset count model gives
    so we can reconcile the exact enumeration convention in the writeup."""
    # paper: branching factor 16, depth 15, first coordinate pinned (x_1 = 0).
    # multiset count with d free positions:
    return {
        "C(B+d-1,d) for B=16,d=15": comb(16 + 15 - 1, 15),
        "C(B+d-1,d) for B=16,d=14": comb(16 + 14 - 1, 14),
        "paper_reported": 77_558_760,
    }


if __name__ == "__main__":
    n = test_exhaustive()
    print(f"[ok] exhaustive rank/unrank vs brute force: {n} vectors verified")
    r = test_paper_example1()
    print(f"[ok] paper Example 1 reproduced: index = {r} (expected 328)")
    print("[info] round-trip unrank(328) =", unrank_fixed(328, 17, 5))
    print("[info] experiment size reconciliation:")
    for k, v in test_experiment_size().items():
        print(f"        {k:32s} = {v:,}")
