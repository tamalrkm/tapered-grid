"""
Warped/selective-branching indexer.

Two independent implementations, cross-checked:

(A) FAITHFUL reproduction of the base paper's selective scheme
    (Biswas-Regan TCS 2015, Eqs. 7/8/6): recursive V-table, survivor
    remapping g, index sum. Verified against the paper's published
    artifacts: Table 7 (the V table) and Example 2 (index 400).

(B) CLEAN nested-alphabet ranker (this paper's generalization):
    alphabets S_1 >= S_2 >= ... >= S_d (reduction at depth t keeps every
    2^{R}-th branch), keys = non-decreasing sequences with m_i in S_i,
    index = lexicographic rank, computed in O(d) lookups from one O(dB)
    DP table (suffix-count table), with unranking. Verified by exhaustive
    enumeration on the Example-2 configuration and on random nested
    configurations.

Also answers: is the paper's selective index a pure lex rank of the
nested key set? (checked empirically below)
"""
from itertools import product

# ----------------------------------------------------------------------
# (A) The paper's Eqs. (7), (8), (6), faithfully
# ----------------------------------------------------------------------
def paper_table(B, d, r):
    """V[i][b], 1-based i (depth), b (branch col). r = binary reduction vector,
    r[0]=r[1]=0. Eq. (7): built recursively for each final depth j; row i of
    the final table is the row computed when j reaches d (intermediate rows
    rewritten per j). R(i,j) = sum_{k=d-j+2}^{d-j+i} r_k   (1-based r)."""
    V = [[0]*(B+2) for _ in range(d+2)]
    for j in range(1, d+1):                     # final depth pass
        Vj = [[0]*(B+2) for _ in range(j+2)]
        for i in range(1, j+1):
            for b in range(1, B+1):
                if i == 1: Vj[i][b] = 0
                elif i == 2: Vj[i][b] = 1
                else:
                    # R(i,j) = number of reductions among absolute depths
                    # (d-j+2 .. d-j+i) mapped onto r (1-based)
                    R = sum(r[k-1] for k in range(d-j+2, d-j+i+1) if 1 <= k <= d)
                    step = 2**R
                    if b + step <= B:
                        Vj[i][b] = Vj[i-1][b] + Vj[i][b+step]
                    else:
                        Vj[i][b] = Vj[i-1][b]
        V[j] = [row[:] for row in Vj]           # keep the pass-j table
    # final table rows: row i of the j=d pass
    return V[d]

def paper_g(h, k, B, r):
    """Eq. (8): map fixed-grid column h (1-based) at depth k to selective col.
    R(k) = sum_{j=1}^{k+1?} -- the paper writes R(k)=sum_{j=1}^{k+1} r_j; with
    r[0]=r[1]=0 and reduction flags aligned so that a reduction AT depth t
    affects mapping FOR depths >= t. We implement R(k) = sum r_j over j<=k."""
    R = sum(r[:k])
    step = 2**R
    if (h - 1) % step != 0:
        return 0                                 # value not present at depth k
    return B - B//step + (h - 1)//step if R > 0 else h

def paper_index(cols_h, B, d, r, V):
    """Eq. (6): index = sum_{i=1}^{d-1} sum_{j=g(u_i,i)}^{g(u_{i+1},i)-1} V[d+1-i][j].
    cols_h = 1-based fixed-grid columns of the key."""
    total = 0
    for i in range(1, d):                        # i = 1..d-1
        a = paper_g(cols_h[i-1], i, B, r)
        b = paper_g(cols_h[i],   i, B, r)
        if a == 0 or b == 0:
            raise ValueError("key not on selective grid")
        for j in range(a, b):
            total += V[d+1-i][j]
    return total

# ----------------------------------------------------------------------
# (B) Clean nested-alphabet ranker (our generalization)
# ----------------------------------------------------------------------
def nested_alphabets(B, d, r):
    """S_i = surviving 1-based fixed columns at depth i: (h-1) % 2^{R(i)} == 0,
    R(i) = reductions at depths <= i."""
    S = []
    for i in range(1, d+1):
        R = sum(r[:i])
        S.append([h for h in range(1, B+1) if (h-1) % (2**R) == 0])
    return S

def enumerate_keys(S):
    """All non-decreasing keys (in fixed-grid column values), lex order."""
    d = len(S)
    out = []
    def rec(i, lo, pref):
        if i == d:
            out.append(tuple(pref)); return
        for v in S[i]:
            if v >= lo:
                rec(i+1, v, pref+[v])
    rec(0, 1, [])
    return out

def suffix_table(S, B):
    """T[i][v] = number of valid completions (m_i..m_d) with m_j in S_j,
    non-decreasing, m_i >= v. 1-based depth i, value v in 1..B+1."""
    d = len(S)
    T = [[0]*(B+2) for _ in range(d+2)]
    for v in range(1, B+2): T[d+1][v] = 1        # empty completion
    for i in range(d, 0, -1):
        # C[v] = sum over s in S_i, s>=v of T[i+1][s]
        for v in range(B, 0, -1):
            T[i][v] = T[i][v+1] + (T[i+1][v] if v in set(S[i-1]) else 0)
    # note: T[i][v] uses T[i+1][s] which already encodes ">= s" chaining? No:
    # completions after choosing m_i=s require m_{i+1} >= s: that is T[i+1][s].
    return T

def nested_rank(key, S, T, B):
    """Lexicographic rank: count keys < key by first point of difference."""
    d = len(S)
    rank = 0
    lo = 1
    for i in range(1, d+1):
        for s in S[i-1]:
            if lo <= s < key[i-1]:
                rank += T[i+1][s] if i < d else 1
            # careful: for i<d completions counted from depth i+1 with m>=s
        lo = key[i-1]
    return rank

def nested_unrank(idx, S, T, B):
    d = len(S)
    key = []
    lo = 1
    for i in range(1, d+1):
        for s in S[i-1]:
            if s < lo: continue
            block = T[i+1][s] if i < d else 1
            if idx < block:
                key.append(s); lo = s; break
            idx -= block
        else:
            raise ValueError("index out of range")
    return tuple(key)

# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------
if __name__ == "__main__":
    # ---- Example 2 of the paper: B=17, d=5, reduction only at depth 4 ----
    B, d = 17, 5
    r = [0, 0, 0, 1, 0]
    V = paper_table(B, d, r)

    # published Table 7 rows
    t7 = {3: [17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1],
          4: [45,36,36,28,28,21,21,15,15,10,10,6,6,3,3,1,1],
          5: [285,240,204,168,140,112,91,70,55,40,30,20,14,8,5,2,1]}
    ok_t7 = all(V[i][1:B+1] == t7[i] for i in t7)
    print(f"[A] paper V-table matches published Table 7: {ok_t7}")

    # example vector u = (0.00, 0.0625, 0.25, 0.50, 0.75) -> fixed cols h:
    # branches equally spaced 0..1 step 1/16: h = 16*value + 1
    u = [0.0, 0.0625, 0.25, 0.50, 0.75]
    cols = [round(16*x)+1 for x in u]            # 1,2,5,9,13
    idx = paper_index(cols, B, d, r, V)
    print(f"[A] paper Example 2 index = {idx}  (published: 400)  "
          f"{'OK' if idx==400 else 'MISMATCH'}")

    # ---- clean nested ranker on the same configuration ----
    S = nested_alphabets(B, d, r)
    keys = enumerate_keys(S)
    T = suffix_table(S, B)
    ok_rank = all(nested_rank(k, S, T, B) == i for i, k in enumerate(keys))
    ok_unrank = all(nested_unrank(i, S, T, B) == k for i, k in enumerate(keys))
    print(f"[B] nested ranker == exhaustive lex enumeration on Example-2 config: "
          f"{ok_rank} (unrank: {ok_unrank}, |keys|={len(keys)})")

    in_grid = tuple(cols) in set(keys)
    lexr = nested_rank(tuple(cols), S, T, B) if in_grid else None
    print(f"[B] example key on nested grid: {in_grid}; pure lex rank = {lexr} "
          f"(paper's index = {idx}; equal: {lexr == idx})")

    # ---- property test: random nested configs ----
    import random
    random.seed(0)
    fails = 0
    for trial in range(30):
        B2 = random.choice([5, 8, 9, 16, 17])
        d2 = random.randint(2, 5)
        r2 = [0, 0] + [random.randint(0, 1) for _ in range(d2-2)]
        r2 = r2[:d2]
        S2 = nested_alphabets(B2, d2, r2)
        if any(len(s) == 0 for s in S2): continue
        ks = enumerate_keys(S2)
        if not ks: continue
        T2 = suffix_table(S2, B2)
        for i, k in enumerate(ks):
            if nested_rank(k, S2, T2, B2) != i or nested_unrank(i, S2, T2, B2) != k:
                fails += 1; break
    print(f"[B] property test over 30 random nested configs: "
          f"{'ALL PASS' if fails==0 else f'{fails} FAILURES'}")
