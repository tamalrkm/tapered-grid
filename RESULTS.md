# Benchmark results

## Go/no-go gate (PASSED) — at-scale C++ head-to-head

`bench.cpp`, g++ -O3 -march=native, this machine (16 GB, 8 cores).
Structure: tapered flat array in a real **mmap'd file** + table-free `O(d)`
address, vs `std::unordered_map` keyed by packed columns. Payload is a cheap
deterministic float (we are measuring the *structure*, not `f`). Same 3M random
sorted queries to both; identical checksums confirm correctness at scale.

| N=37.44M, d=13, B=16, 3M queries | tapered O(d)+mmap | hash memo | ratio |
|---|---|---|---|
| Query latency | **127.8 ns** | 232.3 ns | **1.82× faster** |
| Peak RSS | **294 MB** | 1675 MB | **5.7× smaller** |
| Build | 2.69 s | 8.91 s | 3.3× faster |
| on-disk file | 143 MB (mmap) | — (in-RAM) | — |
| checksum | 1500710.181 | 1500710.181 | equal ✓ |

Interpretation: the Python micro-bench (hash 0.14 µs vs tapered 1.21 µs) inverted
because it pitted a C-level dict against interpreted arithmetic. Compiled and at a
scale where the working set (143 MB array / 1.5 GB map) exceeds L3, the tapered
index does its index math entirely in a tiny L1-resident Pascal table and touches
DRAM exactly once (the value read); the hash touches the bucket array *and* a node
*and* compares keys — ≥2 scattered DRAM reads. Arithmetic beats pointer-chasing.
This is the paper's original thesis ("single index lookup, not pointer jumping"),
now measured. The tapered RSS even *includes* the ~156 MB query buffer, so the
structure-only memory gap is larger than 5.7×.

## Correctness (from refimpl.py / spike_bench.py)
- Exhaustive rank/unrank vs brute force: 6,426 vectors, all `(B,d)` up to `B=8,d=6`.
- Reproduces paper Example 1 index **328**; production file size **C(29,14)=77,558,760**.
- O(d) address == hash memo on all 54,264 gridpoints of a real p1 grid (spike).

## N-scaling sweeps (2026-07-25) — corrects the earlier "latency widens with N" claim

**Sweep varying N via depth d (B=16), 2M queries** — confounds N with query cost:

| N (d) | tapered lat | hash lat | speedup | tapered RSS | hash RSS | mem ratio |
|---|---|---|---|---|---|---|
| 0.49M (8)  | 25.1 | 138.0 | 5.5× | 65  | 84   | 1.3× |
| 3.27M (10) | 84.4 | 164.4 | 1.95×| 91  | 212  | 2.3× |
| 7.73M (11) | 91.3 | 175.0 | 1.92×| 116 | 401  | 3.5× |
| 17.4M (12) | 103.2| 187.1 | 1.81×| 160 | 798  | 5.0× |
| 37.4M (13) | 135.6| 222.3 | 1.64×| 245 | 1626 | 6.6× |

**Clean sweep: FIXED d=10, vary B (query cost constant) — isolates N-scaling:**

| N (B) | tapered lat | hash lat | speedup | tapered RSS | hash RSS | mem ratio |
|---|---|---|---|---|---|---|
| 92K (10)   | 23.5  | 68.6  | 2.9× | 79  | 83   | 1.05× |
| 1.14M (14) | 41.8  | 148.6 | 3.6× | 83  | 126  | 1.5×  |
| 8.44M (18) | 129.8 | 170.3 | 1.31×| 111 | 389  | 3.5×  |
| 44.4M (22) | 119.8 | 169.6 | 1.42×| 248 | 1226 | 4.9×  |

**Honest reading (this REPLACES the earlier "1.8× and widening" latency claim):**
- **Memory** advantage grows **monotonically and without bound** with N (1.05× → 4.9× at
  fixed d=10; 6.6× at d=13). This is the decisive, clean result — the flat array stores
  no keys, the hash's key+node overhead scales with N.
- **Latency**: tapered wins at every scale, but the ratio is **large in-cache (~3×)** then
  **settles to a stable ~1.3–1.8×** once both structures are DRAM-bound (the real
  out-of-core regime). It does NOT widen with N. Mechanism: at scale both pay ~1 DRAM
  miss for the value; tapered saves the hash's extra bucket/node miss + hashing, a
  roughly constant-factor edge.
- Defensible headline: **"memory footprint grows far slower (→ unbounded advantage);
  query latency holds a steady ~1.3–1.8× edge at scale."**

## Warped/selective indexer — VERIFIED (2026-07-27, warped.py)

- Clean nested-alphabet ranker (S_1 ⊇ … ⊇ S_d, O(dB) suffix DP once, O(d)-lookup
  rank + unrank): **exhaustively verified** against lex enumeration on the paper's
  Example-2 config (B=17, d=5, reduction at depth 4; 6,501 keys) and 30 random
  nested configs — rank AND unrank all pass.
- **Reproduces the paper's published Example 2 index = 400 exactly** as the pure
  lexicographic rank of the nested key set — i.e. the selective scheme's index IS
  the lex rank, and our nested-CNS form replaces the paper's recursive Eq. (7)
  table (whose R(i,j) bookkeeping is ambiguous as published — our faithful
  transcription attempt did not reproduce its Table 7; the clean ranker needs
  none of it).

## OUT-OF-CORE (2026-07-27, ooc_bench.cpp) — the flagship experiment ✅

Config: **N = C(36,15) = 5,567,902,560 entries (B=22, d=15), 22.27 GB file — 1.4×
total RAM (16 GB)** — on a rotational HDD (harshest storage case), laptop.

| phase | result |
|---|---|
| build (sequential, chunked write) | 235 s = 24 M entries/s (95 MB/s, disk-bound) |
| **scan-verify (ALL 5.57e9 payloads)** | **199 s, 28.0 M entries/s, 0 MISMATCHES** |
| random point queries (20k, seed 42) | mean 17.0 ms; p50 15.5 ms; p90 25.1 ms; p99 83.9 ms |
| cache/disk split | 10.2% page-cache hits (µs-class); 89.8% disk, disk-mean **18.9 ms** |
| correctness | query checksum == recomputed expectation ✓ |

Reading:
- **One seek per query, exactly** — 18.9 ms disk-mean = a single HDD random read
  (incl. queueing). The O(d) rank costs nothing measurable; there is no second
  dependent access, ever. Deterministic worst case.
- **No key-storing structure can exist here:** measured hash footprint 43.5 B/entry
  → 242 GB RAM at this N (15× this machine). On-disk chained hash = ≥2 dependent
  seeks (≥2× latency) + 4.5× bigger file → ~4.5× smaller cached fraction.
- **Streaming aggregates:** full-table scan at 28 M entries/s (~1000× the random
  rate); arguments implicit by index (unrank only needed for argument-dependent
  filtering). Whole table in 3.3 min on an HDD.
- File deleted after run (rebuild = 4 min via `./ooc_bench build 22 15`).
- NVMe/SSD version of this experiment: bigbox optional task (PROMPT_BIGBOX.md T4).

## BIG-MACHINE results (2026-07-27, dual-EPYC 9335 / 128 threads / 499 GB / NVMe / H200)
Full report: RESULTS_BIGBOX.md. Headlines folded into paper/main.tex.

- **Parallel build via unranking** (N=1.12B–4.08B, /dev/shm, zero coordination):
  realistic `unrank` variant scales ~4.1× by T=64 then hits the DRAM-bandwidth
  ceiling (converges to `flat`'s ~9.5e8 entries/s) — the O(d) reconstruction is
  fully hidden. 0 errors / 1M spot-checks × 32 runs. Vs hash at N=1.12B: flat
  array **183–250× faster build, 10.2× less RAM** (4.2 vs 43.1 GB); 16-shard
  parallel hash only 1.17× (lock/alloc contention). NOTE: scaling is bandwidth-
  limited (~4×), NOT near-linear — honest framing = "builds until bandwidth, not
  threads, is the limit; hash barely parallelizes at all."
- **In-RAM N-sweep to 1.92B** (fixed d=10): memory ratio 5.7×→10.2× (widens,
  unbounded); at fixed d, tapered latency ~flat (99→154 ns) while hash grows
  (111→283 ns) → latency ratio WIDENS 1.1×→1.8× with N. (Reconciles earlier
  laptop note: latency ratio is stable/widening at fixed d, machine-dependent,
  noisy on shared box — reported median of 3.) Hash never OOM'd on 499 GB
  (78 GB peak); "hash can't exist" is the LAPTOP out-of-core story.
- **Cold NVMe query** (N=1.12B, fadvise-evicted): p50 **67 µs**, mean 71 µs;
  warm re-run 1 µs (70× collapse = proof pages were cold). **~230× faster than
  the laptop HDD** (15.5 ms p50) for the identical one-access structure. Clean
  three-tier hierarchy: resident ~100–280 ns / page-cache fresh-mmap ~1 µs /
  cold NVMe ~67–85 µs. (drop_caches blocked — Docker container; fadvise sufficed.)
- **entmax dataset** `entmax_rows.npz` (199,669 GPT-2 rows, top-32 sorted +
  row_len, 23.3 MB, audited sound): **only 0.91% saturate top-32** → exact-tail
  property holds. Solver baselines (CPU, α=1.5): batched entmax15 0.29 µs/row BUT
  **single-row serving 37.6 µs/row (~130×)** — the latter is our fair target for
  per-query lookup. *(Both former caveats are now RESOLVED — see the WikiText
  regeneration below; this bigbox dataset is superseded.)*

### entmax dataset REGENERATED on WikiText-2 (2026-07-28, laptop) — caveats cleared

The bigbox dataset was built on this repo's own docs because `datasets` 5.0.0
rejected the `wikitext` repo id. Regenerated on the laptop with the canonical
corpus; **both caveats from the bigbox run are now closed.**

- Blocker was NOT the corpus (`datasets` 2.20.0 here loads `wikitext-2-raw-v1`
  fine, 36,718 rows). It was `transformers` eagerly importing a **torchvision
  0.21.0+cu124 built against torch 2.6** under **torch 2.11.0+cu130** →
  `RuntimeError: operator torchvision::nms does not exist`. GPT-2 is text-only,
  so `src_dump_entmax_rows.py` now probes torchvision and sets
  `transformers.utils.import_utils._torchvision_available = False` when it is
  broken. **No package was installed, upgraded, or removed.**
- New dataset: **199,741 rows**, 98×512 = 50,176 WikiText-2 tokens, 2.77%
  sampling over (layer,head,position), seed 0. Integrity: no NaN, desc-sorted,
  row_len consistent; row_len min 2 / max 512 / mean 257.1.
- Support profile essentially unchanged → **the negative result is corpus-robust**:
  median 2, mean **4.01** (was 4.2), p99 29, **0.88% saturate top-32** (was 0.91%);
  exact-vs-bisection θ agreement 1.0e-15.
- Benchmark `./entmax_bench 16 12 2.0` (|U|=7.73M, 30.9 MB, 199,741 rows):
  θ err mean **3.76e-3**, p99 2.29e-2, max 8.96e-2; **bias +1.97e-5 = 0.005×
  mean|err|** (bias lemma again confirmed, even tighter than the docs run);
  downstream max|Δp| mean 5.65e-3. Latency **memo 92 ns vs exact 35 ns
  (0.4×, i.e. memo loses by 2.6×)**, bisect 583 ns (6.4×).
- Prior docs-corpus artifacts kept for comparison: `entmax_rows_docs.npz.bak`,
  `entmax_u_docs.bin.bak`. Full run log: `entmax_dump.log`.

## APPLICATION BENCHMARKS (2026-07-27, laptop, C++ -O3) — one WIN, one honest LOSS

**Methodology note:** an earlier run had a bug in the nearest-grid interpolation
(the scan started one column high, rounding every coordinate up → bias ≡ mean
error). Fixed in `pl_bench.cpp`/`entmax_bench.cpp`; accuracy improved ~13× and
the bias dropped to a fraction of the mean error, as the bias lemma predicts.
All numbers below are post-fix. Latencies are same-language (C++ vs C++),
single-query (the realistic serving pattern).

### Plackett–Luce / vectorized root — **PRIMARY APPLICATION, memo WINS**
`f(x)=p1` solving `Σ p1^{1/(1-x_i)} = 1`; no closed form, every exact call is an
iterative root-find with 32 `pow()` per iteration. Queries from the **2015
paper's own generator** (b=5), 200k queries, K=32.

| grid | table | mean err | p99 | max | bias/mean|err| | memo | Newton | bisect | speedup |
|---|---|---|---|---|---|---|---|---|---|
| B=12,d=8  | 0.1 MB | 1.30e-2 | 3.4e-2 | 5.5e-2 | 0.99 | 70 ns | 3872 ns | 28434 ns | **55×** / 406× |
| B=16,d=12 | 31 MB  | 3.04e-3 | 1.1e-2 | 2.3e-2 | 0.34 | 117 ns | 3797 ns | 28417 ns | **33×** / 244× |
| B=20,d=12 | 218 MB | 2.64e-3 | 9.4e-3 | 1.9e-2 | 0.47 | 145 ns | 3811 ns | 28899 ns | **26×** / 199× |
| B=16,d=16 | 620 MB | 2.65e-3 | 9.1e-3 | 1.9e-2 | 0.30 | 149 ns | 3780 ns | 28286 ns | **25×** / 189× |

- **26–55× over Newton, 189–406× over bisection** — and note the 2015 paper
  claimed only "~10× faster"; the table-free index does substantially better.
- Accuracy saturates at ≈2.6e-3 mean (interpolation/truncation floor, nearest-
  grid = the paper's "NN" strategy; their credit-weighted scheme reached ~1e-3).
- **Bias is 0.30–0.47× the mean error** → the bias lemma confirmed empirically;
  this is what licenses the aggregate/mean use case.

### α-entmax threshold — **HONEST NEGATIVE RESULT: memo LOSES**
Real GPT-2 rows on **WikiText-2 (raw)** — 199,741 rows, α=1.5, top-32 gaps.
*(Numbers below are the WikiText regeneration of 2026-07-28; the earlier
docs-corpus run gave 86 ns / 3.9e-3 / 0.014× / 2.5× / 6.6× — i.e. the same
conclusion, which is the point: the result is corpus-robust.)*

| | latency | note |
|---|---|---|
| memo (snap+rank+read) | 92 ns | accuracy fine: θ err mean 3.76e-3, bias **0.005×** mean|err| |
| **exact sorted-support** | **35 ns** | **2.6× FASTER than the memo table** |
| naive bisection (60 it) | 583 ns | memo beats this 6.4×, but nobody should use it |

**Why:** the exact α-entmax threshold is a closed form over the *active support*,
and our own profiling of the real rows shows support is tiny — **median 2, mean
4.01**; only 0.88% saturate top-32. An O(support≈2) closed form cannot be beaten
by an O(d) interpolate + O(d) rank + memory access.
**The 37.6 µs/row bigbox figure was PyTorch per-call overhead, not intrinsic
cost** — in C++ the same math is 35 ns. Lesson (worth stating in the paper):
memoize only when exact evaluation is genuinely *iterative/transcendental*
(Plackett–Luce: 3800 ns), not merely "a solve" (entmax: 35 ns).

## Still to run for the SEA submission
- [ ] Add baselines to the matrix: **base-paper table-based index** (does table-free
      match its latency while dropping the `O(Bd²)` build/memory?), **kd-tree**
      (scipy already showed the memory blow-up; add compiled latency).
- [ ] **Cold-cache / true out-of-core**: file > RAM, `madvise(RANDOM)`, report the
      regime where the map cannot reside in RAM at all (the paper's real use case).
- [ ] **End-to-end app speedup**: real expensive `f` (iterative-h Plackett-Luce p1)
      at scale — revisit the paper's "~10× vs recomputation" claim with error bars.
- [ ] Warped/selective indexer at scale; reproduce Example 2 index **400** first.
- [ ] Sweep N (1M→100M) and d to show the crossover and the widening gap.
