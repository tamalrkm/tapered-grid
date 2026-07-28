"""
Step 1 for the entmax application benchmark: exact alpha=1.5 threshold solver,
cross-checked two ways, then profile the REAL GPT-2 rows to size the memo grid.

alpha=1.5 entmax: p_i = [(alpha-1) z_i - tau]_+^{1/(alpha-1)} = [0.5 z_i - tau]_+^2,
sum p = 1. Shift-invariant: set u_i = (alpha-1)(z_1 - z_i) = 0.5 (z_1 - z_i) >= 0,
non-decreasing, u_1 = 0. Then p_i = [theta - u_i]_+^2 with theta solving
    S(theta) := sum_i (theta - u_i)_+^2 = 1,      support = {i : u_i < theta}.
theta in (0, 1]: all-tied k entries give k*theta^2=1 -> theta=1/sqrt(k) <= 1, and
support entries have u_i < theta <= 1, so only u in [0,1] matters; u_i >= 1 is
guaranteed zero influence (the exact tapered tail).
"""
import numpy as np
d = np.load('entmax_rows.npz')
top32 = d['top32'].astype(np.float64)          # (N,32) sorted desc, -inf padded
N = top32.shape[0]
ALPHA = 1.5

# u_i = 0.5*(z_1 - z_i); z_1 is column 0 (largest). -inf -> +inf gap -> clamp later.
u = (ALPHA - 1.0) * (top32[:, :1] - top32)      # (N,32), u[:,0]=0, nondecreasing
u[~np.isfinite(u)] = np.inf

def theta_exact(urow):
    """Exact O(support): try support sizes k=1,2,... find valid theta."""
    S1 = 0.0; S2 = 0.0
    for k in range(1, len(urow) + 1):
        uk = urow[k-1]
        if not np.isfinite(uk):                 # padding => support < k
            break
        S1 += uk; S2 += uk*uk
        disc = S1*S1 - k*(S2 - 1.0)
        if disc < 0:
            continue
        theta = (S1 + np.sqrt(disc)) / k
        # valid iff u_{k-1} < theta <= u_k (next); check boundary
        nxt = urow[k] if k < len(urow) and np.isfinite(urow[k]) else np.inf
        if uk < theta <= nxt + 1e-12:
            return theta, k
    # all finite entries in support
    fin = urow[np.isfinite(urow)]
    k = len(fin); S1 = fin.sum(); S2 = (fin*fin).sum()
    return (S1 + np.sqrt(max(S1*S1 - k*(S2-1.0), 0.0)))/k, k

def theta_bisect(urow, iters=60):
    """Bisection on theta in [0,1] of S(theta)=1 (monotone increasing)."""
    lo, hi = 0.0, 1.0
    uf = urow.copy(); uf[~np.isfinite(uf)] = 1e9
    for _ in range(iters):
        t = 0.5*(lo+hi)
        s = np.sum(np.clip(t - uf, 0, None)**2)
        if s < 1.0: lo = t
        else: hi = t
    return 0.5*(lo+hi)

# cross-check exact vs bisection on 5000 rows
rng = np.random.default_rng(0)
idx = rng.choice(N, 5000, replace=False)
te = np.array([theta_exact(u[i])[0] for i in idx])
tb = np.array([theta_bisect(u[i]) for i in idx])
print(f"exact vs bisection theta: max|diff| = {np.abs(te-tb).max():.2e}  (want <1e-6)")

# support-size + theta distribution over the sample
ks = np.array([theta_exact(u[i])[1] for i in idx])
print(f"\nsupport size (# nonzero entmax entries):")
for p in [50, 90, 95, 99, 99.9, 100]:
    print(f"  p{p:>5}: {np.percentile(ks, p):.0f}")
print(f"  max = {ks.max()}, mean = {ks.mean():.2f}")
print(f"  fraction with support > 8:  {100*np.mean(ks>8):.2f}%")
print(f"  fraction with support > 16: {100*np.mean(ks>16):.2f}%")
print(f"  fraction with support = 32 (saturates top-32): {100*np.mean(ks>=32):.2f}%")

print(f"\ntheta (the value we memoize): min={te.min():.3f} max={te.max():.3f} "
      f"mean={te.mean():.3f}")
# how deep do the INFLUENTIAL gaps go? largest u_i that is still < theta (in support)
maxgap = []
for i in idx[:2000]:
    th, k = theta_exact(u[i])
    maxgap.append(u[i][k-1])                     # last in-support gap
maxgap = np.array(maxgap)
print(f"largest in-support gap u_i: p50={np.percentile(maxgap,50):.3f} "
      f"p99={np.percentile(maxgap,99):.3f} max={maxgap.max():.3f}  (all < theta <= 1)")
