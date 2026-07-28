"""
Verify the CORRECTED finite-eps mismatch-penalty formula against the exact
numerical optimum from verify_schedule.py.

Derivation summary (product-cost model, g_i = r^(i-1), error sum g_i/(2 n_i)):

OPTIMAL (water-filling): k active levels, equalized error e* ~ eps/k, boundary
r^(k-1) ~ 2e*  =>  k*a = L' + ln k + O(1),   a = ln(1/r), L' = ln(1/(2 eps)).
ln|U*| = (a/2) k^2 + O(k).

GEOMETRIC ratio s (n_i = n1 s^(i-1), truncated at n_i >= 1, minimal feasible n1):
- s < r: per-level error grows to the truncation boundary; total error
  ~ (r^(m-1)/2) * C_<,  C_< = 1/(c*(1 - s/r)) + r/(1-r), c = n1 s^(m-1) in [1,1/s)
  =>  m*a = L' + ln C_< + O(1),  ln|U_s| = (b/2) m^2 + O(m),  b = ln(1/s).
- s > r: error dominated by level 1: n1 ~ (1/(2 eps)) / (1 - r/s)
  =>  ln n1 = L' + ln C_>,  C_> = 1/(1 - r/s),  m = 1 + ln(n1)/b,
  ln|U_s| = (b/2) m^2 + O(m).

UNIFIED CORRECTED FORMULA:
  rho(r,s,eps)  =  rho_inf(s) * [ (L' + ln C_branch) / (L' + ln k) ]^2  + O(1/L')
  rho_inf(s)    =  max( b/a, a/b )         (the classical asymptotic penalty)

So rho -> rho_inf as eps -> 0, but LOG-SLOWLY: the bracket -> 1 like
(1 - (ln k - ln C)/L')^2.  At practical eps the classical formula overstates
the penalty substantially (e.g. 2.41 predicted vs ~1.88 actual at eps=1e-8).
"""
import math
from verify_schedule import optimal_size, geometric_size

def corrected_rho(r, s, eps, cbar=1.35):
    a, b = math.log(1/r), math.log(1/s)
    Lp = math.log(1/(2*eps))
    # k from k*a = L' + ln k (fixed point)
    k = Lp/a
    for _ in range(50):
        k = (Lp + math.log(k))/a
    if s < r:
        C = 1.0/(cbar*(1 - s/r)) + r/(1 - r)
    else:
        C = 1.0/(1 - r/s)
    rho_inf = max(b/a, a/b)
    bracket = (Lp + math.log(C))/(Lp + math.log(k))
    return rho_inf * bracket*bracket, rho_inf

print(f"{'r':>5} {'s':>5} {'eps':>7} | {'rho_emp':>8} {'rho_corr':>9} {'err%':>6} | {'rho_inf':>8} {'err%':>7}")
print("-"*70)
for r, s in [(0.75,0.5),(0.75,0.9),(0.75,0.6),(0.75,0.4),
             (0.5,0.25),(0.5,0.75),(0.5,0.6),(0.5,0.4),
             (0.25,0.5),(0.25,0.6),(0.25,0.75)]:
    for eps in (1e-6, 1e-8, 1e-10):
        ln_opt, k_emp, e = optimal_size(r, eps)
        ln_s = geometric_size(r, eps, s)
        emp = ln_s/ln_opt
        corr, inf_ = corrected_rho(r, s, eps)
        e1 = 100*(corr-emp)/emp
        e2 = 100*(inf_-emp)/emp
        print(f"{r:>5.2f} {s:>5.2f} {eps:>7.0e} | {emp:>8.3f} {corr:>9.3f} {e1:>+6.1f} | {inf_:>8.3f} {e2:>+7.1f}")
