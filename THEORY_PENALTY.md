# The corrected finite-ε mismatch penalty (product-cost model)

Status: **DERIVED** (two-term asymptotic, clean derivation below) +
**NUMERICALLY VERIFIED** (`verify_penalty.py`: 1–3% typical error vs exact
optimum, vs 16–43% for the classical asymptotic formula). Replaces the loose
`ρ∞(s)` claim in THEORY.md §6.

## Setup

Influence `g_i = r^{i-1}`; level `i` uses `n_i` points; error `Σ g_i/(2n_i) ≤ ε`;
minimize `ln|U| = Σ ln n_i`. Notation: `a = ln(1/r)`, `b = ln(1/s)`,
`L' = ln(1/(2ε))`.

## Optimal schedule (water-filling) — the `ln k` depth bonus

Equalized per-level error `e* ≈ ε/k` over `k` active levels; the activation
boundary `r^{k-1} ≈ 2e* = 2ε/k` gives the fixed point

```
k·a = L' + ln k + O(1)          and   ln|U*| = (a/2)k² + O(k).
```

**Key mechanism:** because `e* = ε/k` *shrinks as more levels activate*, the
optimum earns an extra `ln k` of effective budget — a "depth bonus" that any
fixed-ratio schedule cannot collect.

## Geometric schedule `n_i = n₁ s^{i-1}` (truncated at `n_i ≥ 1`, minimal feasible `n₁`)

- **`s < r`** (refining too fast): per-level error grows toward the truncation
  boundary; total error `≈ (r^{m-1}/2)·C_<` with
  `C_< = 1/(c̄(1−s/r)) + r/(1−r)` (`c̄ = n₁s^{m-1} ∈ [1,1/s)`, mean ≈ 1.35), so
  `m·a = L' + ln C_< + O(1)`, `ln|U_s| = (b/2)m² + O(m)`.
- **`s > r`** (refining too slowly): error dominated by level 1:
  `n₁ ≈ (1/2ε)·C_>`, `C_> = 1/(1−r/s)`, so `m = 1 + ln n₁/b` and again
  `ln|U_s| = (b/2)m² + O(m)`.

## The corrected penalty

```
                       ⎡ L' + ln C_branch ⎤²
ρ(r,s,ε) = ρ∞(s) · ⎢───────────────────⎥   + O(1/L'),      ρ∞(s) = max(b/a, a/b)
                       ⎣ L' + ln k        ⎦
```

- **ε→0 limit recovers the classical `ρ∞(s)`** — that formula was right as an
  asymptote, but convergence is **logarithmically slow** (bracket → 1 like
  `(1 − (ln k − ln C)/L')²`), so at any practical ε it substantially overstates
  the penalty. Since `ln k > ln C` in all tested regimes, the finite-ε penalty is
  systematically *below* `ρ∞` — mis-tapering hurts, but less than the asymptote says.
- Interpretation: the bracket is exactly the (squared) ratio of the two schedules'
  effective depths — optimal gets `L' + ln k`, geometric only `L' + ln C`.

## Numerical verification (`verify_penalty.py`, exact optimum via `verify_schedule.py`)

| (r, s) | ε | ρ_emp | ρ_corrected (err) | ρ∞ classical (err) |
|---|---|---|---|---|
| (0.75, 0.50) halving | 1e-8 | 1.878 | 1.858 (−1.0%) | 2.409 (+28%) |
| (0.75, 0.90) | 1e-8 | 2.110 | 2.136 (+1.2%) | 2.730 (+29%) |
| (0.50, 0.25) | 1e-8 | 1.578 | 1.554 (−1.5%) | 2.000 (+27%) |
| (0.50, 0.75) | 1e-8 | 1.866 | 1.910 (+2.3%) | 2.409 (+29%) |
| (0.25, 0.50) halving | 1e-8 | 1.569 | 1.628 (+3.8%) | 2.000 (+27%) |
| (0.25, 0.75) | 1e-8 | 3.585 | 3.801 (+6.0%) | 4.819 (+34%) |

Across 33 (r,s,ε) cells: corrected err 0.6–8.2% (median ~1.5%), **improving as
ε→0** in every cell (confirms the two-term form); classical err +16% to +43%.
Residual is the dropped `O(m)` linear terms (`m·ln c̄`, `−bm/2`, `θ`-rounding) —
largest where `k` is small (r=0.25 → k=15). A three-term version would need the
exact `c̄` distribution; not worth it for a design-guidance paragraph.

**Qualitative conclusions unchanged and now quantitatively correct:**
`s = r` optimal; halving optimal iff `r = ½`; e.g. at `r = 0.75`, ε=1e-8, halving
inflates `ln|U|` by **1.88×** (not the asymptotic 2.41×).

## Second-order bias lemma — DRAFTED (2026-07-27)

**Lemma (bias is one order smaller than error).** Let `f ∈ C²` on a grid cell and
let the query `x` fall in the cell of gridpoint `u` under nearest-gridpoint
rounding with per-level spacing `δ_i`. If the query density is `C¹` (locally
approximately constant on the fine cell), then

```
E[f(x) − f(u)]  =  Σ_i (∂²f/∂x_i²)(u) · δ_i²/24  +  O(Σ_i δ_i³ + Σ_i δ_i²·|ρ'/ρ|)
```

so `|bias| ≤ (1/24)·Σ_i M_i δ_i² + h.o.t.` with `M_i = sup|∂²f/∂x_i²|`, while the
worst-case error is first order, `Σ_i g_i δ_i/2`. Hence bias/error → 0 as the grid
refines: **averages over many queries converge at the second-order rate**, which is
what the aggregate use cases (IPR fitting, batch entmax statistics) rely on.

*Proof sketch (PROVED at this level).* Taylor within the cell:
`f(x) − f(u) = ∇f(u)·(x−u) + ½(x−u)ᵀH(ξ)(x−u)`. Under exactly-uniform per-cell
query measure, `E[x_i−u_i] = 0` kills the first-order term and
`E[(x_i−u_i)(x_j−u_j)] = δ_ij δ_i²/12` gives the constant. For a general `C¹`
density, the within-cell mean offset is `O(δ_i²·(ln ρ)')`, so the first-order term
contributes at second order too — same conclusion, larger constant. ∎

**Honest caveats (state in the paper):**
1. Coordinates in one-sided cells — the monotone-boundary cells and the truncated
   tail levels (`n_i = 1`, rounding to the padded ceiling) — have first-order bias;
   in the optimal taper these levels have `g_i ≤ 2ε/k`, so their total contribution
   stays within the error budget, but they do NOT enjoy the δ² rate.
2. The 2015 paper's balanced-credit interpolation is deterministic, not per-cell
   symmetric; the lemma covers nearest-gridpoint rounding. The credit scheme's
   near-zero empirical means (its Table 11) are consistent with, but not covered
   by, this lemma — say so.
3. This corrects the 2015 empirical "unbiasedness" claim: the truth is O(δ²)
   bias, not exact zero (curvature term survives).
4. Novelty: none claimed — Bennett-lineage high-resolution quantization folklore
   (Bennett 1948), applied. Cite, state, move on.

## Task C — other attainable guarantees, ranked by value/effort for JEA

1. **Second-order bias lemma (RECOMMENDED, ~1 page).** For nearest-gridpoint
   rounding and any query distribution symmetric within cells, `E[f(u)−f(x)] =
   O(Σ g'_i δ_i²)` while worst-case error is `O(Σ g_i δ_i)` — i.e. **bias is one
   order smaller than error**, which is what justifies the aggregate/mean use
   case (chess IPR fitting, entmax batch statistics). Proof: Taylor + symmetric
   rounding; the first-order term has zero mean within each cell. Honesty flag:
   this is standard high-resolution quantization folklore (Bennett 1948 lineage)
   *applied* to our structure — state as a lemma with citation, do not claim
   novelty. Note the 2015 paper claims unbiasedness only empirically; exact
   zero bias is FALSE in general (curvature), the correct claim is O(δ²).
2. **The finite-ε penalty above** (this file) — done.
3. Second-order (curvature) worst-case error bound — needs `|∂²f|` bounds;
   routine, adds little for JEA. SKIP unless a reviewer asks.
4. High-probability error under a query distribution — needs distributional
   assumptions we'd have to defend; the bias lemma already covers the
   aggregate story. SKIP.
