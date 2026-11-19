"""
Numerical check of the taper-mismatch claims of Appendix B of the paper, and the
source of its Table 2.

Model: influence g_i = r^(i-1); depth i uses n_i grid points, per-coordinate
worst-case error g_i/(2 n_i); total error budget sum_i g_i/(2 n_i) <= eps.
Minimize grid size |U| = prod_i n_i.

Checks:
 (1) OPTIMAL (continuous): inverse water-filling gives EQUAL per-active-level
     error e*, n_i = g_i/(2 e*). Verify it beats other feasible schedules.
 (2) For a geometric schedule n_i = n1 * s^(i-1) (truncated at n_i>=1),
     ln|U_s| / ln|U*| -> rho(s) = max(ln(1/s)/ln(1/r), ln(1/r)/ln(1/s)),
     minimized (=1) at s=r. In particular "halving" (s=1/2) is optimal iff r=1/2.
 (3) Table 2 of the paper: the same ratio at eps = 1e-3, printed last.
"""
import math

def optimal_size(r, eps, dmax=100000):
    # water-filling: find e* with sum_i min(e*, g_i/2) = eps, g_i=r^(i-1)
    g = [r**i for i in range(dmax)]          # g_{i+1}
    lo, hi = 0.0, max(g[0]/2, eps)
    for _ in range(200):
        e = 0.5*(lo+hi)
        s = sum(min(e, gi/2) for gi in g)
        if s < eps: lo = e
        else: hi = e
    e = 0.5*(lo+hi)
    ln_size = 0.0; k = 0
    for gi in g:
        ni = gi/(2*e)
        if ni > 1: ln_size += math.log(ni); k += 1
        else: break
    return ln_size, k, e

def geometric_size(r, eps, s, dmax=100000):
    # n_i = n1 * s^(i-1), truncated where n_i<1; pick minimal feasible n1 (bisect).
    # error(n1) = (1/(2 n1)) * sum_{i: n_i>=1} (r/s)^(i-1)  is decreasing in n1.
    def err(n1):
        tot = 0.0; i = 0
        while True:
            ni = n1 * s**i
            if ni < 1: break
            tot += (r**i)/(2*ni)          # = (1/(2n1))*(r/s)^i
            i += 1
            if i > dmax: break
        # coordinates past truncation use n_i=1 -> add their residual error
        tail = sum(r**j/2 for j in range(i, min(i+2000, dmax)))
        return tot + tail
    lo, hi = 1.0, 1.0
    while err(hi) > eps: hi *= 2
    for _ in range(200):
        mid = 0.5*(lo+hi)
        if err(mid) > eps: lo = mid
        else: hi = mid
    n1 = hi
    ln_size = 0.0; i = 0
    while True:
        ni = n1 * s**i
        if ni < 1: break
        ln_size += math.log(ni); i += 1
    return ln_size

def rho_theory(r, s):
    a, b = math.log(1/s)/math.log(1/r), math.log(1/r)/math.log(1/s)
    return max(a, b)

for r in (0.75, 0.5, 0.25):
    eps = 1e-8
    ln_opt, k, e = optimal_size(r, eps)
    print(f"\n=== r={r}  eps={eps}  ===  optimal: ln|U*|={ln_opt:.3f}  active levels k={k}")
    print(f"  {'s':>6} {'ln|U_s|':>10} {'rho_emp':>9} {'rho_thy':>9}  {'note':<20}")
    for s in (0.9, 0.75, 0.6, 0.5, 0.4, 0.25):
        ln_s = geometric_size(r, eps, s)
        rho_emp = ln_s/ln_opt
        note = "<-- s=r (optimal)" if abs(s-r)<1e-9 else ("<-- halving" if abs(s-0.5)<1e-9 else "")
        print(f"  {s:>6.2f} {ln_s:>10.3f} {rho_emp:>9.3f} {rho_theory(r,s):>9.3f}  {note:<20}")

# Table 2 of the paper: inflation of ln|U| by a mismatched refinement ratio at eps = 1e-3.
eps = 1e-3
print(f"\n=== Table 2: ln|U_s| / ln|U*| at eps={eps} ===")
print(f"  {'r':>5} " + " ".join(f"s={s:<5.2f}" for s in (0.25, 0.5, 0.75)))
for r in (0.25, 0.5, 0.75):
    ln_opt = optimal_size(r, eps)[0]
    print(f"  {r:>5.2f} " + " ".join(f"{geometric_size(r, eps, s) / ln_opt:>7.2f}" for s in (0.25, 0.5, 0.75)))
