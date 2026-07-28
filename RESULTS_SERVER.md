# server results — tapered grid paper (bigger-machine experiments)

## Summary (key numbers for the paper)

1. **Parallel construction via unranking (Task 1)**: zero-coordination
   parallel build (worker j fills `[jN/T,(j+1)N/T)` directly via `unrank`,
   no locks) scales **~4.1× by T=64** at N≈1e9–4e9 before hitting this box's
   memory-bandwidth ceiling — at which point it matches the trivial-payload
   fill exactly, showing the O(d) unranking cost is fully hidden by
   parallelism. Vs. the fair "best simple parallel hash" (16 shard maps, 16
   threads): the flat array builds **~183–250× faster** and uses **~10.2×**
   less memory at N=1.12e9, while 16-way sharding only buys the hash a
   **1.17×** speedup (lock + allocator contention dominate).
2. **Extended N-sweep (Task 2)**: at N up to 1.92e9 (20× the original 16GB-
   laptop spike's largest N), the tapered structure's advantage over hash
   **widens** with N on both axes measured — latency ratio 1.1×→1.8×, memory
   ratio 5.7×→10.2× — confirming RESULTS.md's laptop-scale trends hold, and
   showing the hash baseline (78 GB peak here) simply never OOMs on a RAM-rich
   box the way it would on the original 16 GB laptop.
3. **α-entmax application (Task 3 — now the paper's primary application)**:
   exact sorted-support solver (entmax-package-style closed form) sanity-
   checked to 9.2e-15 max prob. error vs. the real `entmax` package (required
   <1e-6). The laptop memo table's real target baseline is **37.6 μs/row**
   (realistic one-row-at-a-time serving latency for `entmax15` on CPU) — the
   batched-vectorized number (0.29 μs/row) is ~130× more optimistic and only
   applies if whole score matrices are processed in bulk. Only 0.91% of rows
   saturate the top-32 window, confirming the assumed "exact tail" property
   holds in practice. **`entmax_rows.npz` was never skipped** (GPU was
   available throughout) and has since been re-audited end to end — see the
   follow-up addendum at the bottom of this file.
4. **Task 4 (cold-cache NVMe query latency)**: system-wide `drop_caches` is
   blocked (this box is a Docker container — `/proc/sys` is read-only by
   design, even for root), but `ooc_bench`'s own `posix_fadvise(DONTNEED)`
   achieved genuine cache eviction anyway, confirmed by a warm-rerun control
   (71µs cold → 1µs warm, ~70× collapse). Cold NVMe: **p50=67µs, mean=71µs**
   vs. the laptop's HDD **p50=15.5ms, disk-mean=18.9ms** — a **~230–266×**
   speedup, the headline contrast for the paper. See the follow-up addendum
   for the full three-tier latency hierarchy (tmpfs-mapped / warm-page-cache
   / cold-NVMe) this reveals alongside Tasks 1–2.

Full detail, raw CSVs/logs, and methodology caveats (shared/noisy box, one
dataset substitution) are below, task by task.

## Hardware / software

- CPU: 2× AMD EPYC 9335 32-Core Processor (128 logical CPUs, SMT2, 64 physical
  cores/socket... i.e. 32 cores × 2 sockets × 2 threads = 128 logical).
- NUMA: 2 nodes (node0: CPUs 0-31,64-95; node1: CPUs 32-63,96-127).
- Cache: L1d 3 MiB (64 instances, i.e. 48 KiB/core), L1i 2 MiB, L2 64 MiB total
  (1 MiB/core), L3 256 MiB (8 instances, i.e. 32 MiB/CCD-ish shared).
- CPU max boost: 4.42 GHz.
- RAM: 499 GB total, ~415 GB available at start of run (83 GB already in use by
  other tenants of this shared box; noted so RAM-hungry baselines size correctly).
- Swap: 7 GB total (mostly avoid relying on it).
- GPU: NVIDIA H200 NVL, 143.8 GB HBM, ~40 GB already in use by another
  process/tenant at the time of measurement (`nvidia-smi` showed no owning PID —
  likely another container/MIG-less multi-tenant usage), driver 580.126.20,
  CUDA 13.1. ~103 GB free for our use.
- Disk: root/home on LVM over 2× Samsung MZWLR3T8 enterprise NVMe/SSD (3.5 TB
  each, non-rotational), 1.5 TB free. `/dev/shm` (tmpfs) has 63 GB.
- OS: Linux 7.0.0-15-generic (Ubuntu 24.04 userland).
- Compiler: gcc/g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1).
- Python: 3.12.3, managed via `uv` per project convention.

Notes on methodology:
- This box is shared (pre-existing RAM/GPU usage from other tenants at
  baseline) — not a dedicated bare-metal box. Sizing decisions below (which N
  to attempt for the hash baseline, GPU batch sizes) account for the ~415 GB
  RAM / ~103 GB GPU actually available rather than the nominal 499 GB / 144 GB.
## Task 1 — Parallel construction via unranking (headline experiment)

`build_parallel.cpp` (new file, compiled `g++ -O3 -march=native -pthread`).
Grid sizes chosen with `refimpl.py`'s `total_count(B,d)`, keeping **d=10 fixed**
across both so the only thing that changes is B (and, later, so this lines up
with Task 2's fixed-d=10 sweep):
- **N≈1e9**: B=32, d=10 → C(41,10) = **1,121,099,408**.
- **N≈4e9**: B=37, d=10 → C(46,10) = **4,076,350,421** (RAM ≥64GB on this box,
  so we ran the larger size too).

Two build variants, both with **zero inter-thread coordination** (worker j
just fills `[j·N/T, (j+1)·N/T)`; no locks, no shared state):
- **flat**: `arr[r] = value_of(r)` directly — payload only needs the index.
- **unrank**: worker actually calls `unrank_fixed(r)` to recover the column
  vector, then computes a payload from the columns (sum-of-columns hashed) —
  the realistic case where `f` needs the argument, not just its rank.

**Correctness**: after every build, 1,000,000 random indices were checked —
`flat` against `value_of(i)` directly, `unrank` via the round-trip
`rank_fixed(unrank_fixed(i)) == i` plus payload self-consistency. **0/1,000,000
errors on every one of the 32 runs below** (both N, both modes, all 8 thread
counts).

### T-sweep, N=1,121,099,408 (B=32, d=10), /dev/shm mmap

| T | flat build_s | flat entries/s | flat speedup (eff.) | unrank build_s | unrank entries/s | unrank speedup (eff.) |
|---|---|---|---|---|---|---|
| 1   | 0.938 | 1.196e9 | 1.00x (100%) | 4.851  | 2.311e8 | 1.00x (100%) |
| 2   | 0.900 | 1.246e9 | 1.04x (52%)  | 2.902  | 3.863e8 | 1.67x (84%)  |
| 4   | 1.097 | 1.022e9 | 0.85x (21%)  | 1.793  | 6.251e8 | 2.70x (68%)  |
| 8   | 1.442 | 7.776e8 | 0.65x (8%)   | 1.353  | 8.284e8 | 3.58x (45%)  |
| 16  | 1.223 | 9.168e8 | 0.77x (5%)   | 1.266  | 8.857e8 | 3.83x (24%)  |
| 32  | 1.184 | 9.469e8 | 0.79x (2%)   | 1.189  | 9.428e8 | 4.08x (13%)  |
| 64  | 1.174 | 9.552e8 | 0.80x (1%)   | 1.169  | 9.593e8 | 4.15x (6%)   |
| 128 | 1.282 | 8.742e8 | 0.73x (1%)   | 1.271  | 8.824e8 | 3.82x (3%)   |

### T-sweep, N=4,076,350,421 (B=37, d=10), /dev/shm mmap

| T | flat build_s | flat entries/s | flat speedup (eff.) | unrank build_s | unrank entries/s | unrank speedup (eff.) |
|---|---|---|---|---|---|---|
| 1   | 3.199  | 1.274e9 | 1.00x (100%) | 17.297 | 2.357e8 | 1.00x (100%) |
| 2   | 3.305  | 1.233e9 | 0.97x (48%)  | 10.436 | 3.906e8 | 1.66x (83%)  |
| 4   | 3.976  | 1.025e9 | 0.80x (20%)  | 6.243  | 6.530e8 | 2.77x (69%)  |
| 8   | 5.338  | 7.637e8 | 0.60x (7%)   | 4.981  | 8.184e8 | 3.47x (43%)  |
| 16  | 4.680  | 8.711e8 | 0.68x (4%)   | 4.594  | 8.873e8 | 3.77x (24%)  |
| 32  | 4.342  | 9.389e8 | 0.74x (2%)   | 4.401  | 9.263e8 | 3.93x (12%)  |
| 64  | 4.292  | 9.498e8 | 0.75x (1%)   | 4.276  | 9.534e8 | 4.05x (6%)   |
| 128 | 4.678  | 8.714e8 | 0.68x (3%)   | 4.701  | 8.672e8 | 3.68x (3%)   |

**Reading (this is the paper's headline mechanism, measured):**
- `flat` is **already memory-bandwidth-bound at T=1** (~1.2–1.3 GB·entries/s ≈
  4.8–5 GB/s of pure sequential store): more threads barely help and sometimes
  *hurt* (a reproducible dip at T=8, confirmed by 3 repeat runs — build_s
  1.449, 1.459, 1.449s vs the T=4 value of 1.10s; not noise). This single core
  is already near this box's practical streaming-write ceiling for a trivial
  payload.
- `unrank` — the realistic variant that actually reconstructs the argument —
  is **compute-bound at low T and scales close to linearly**: 1.67× at T=2,
  2.70–2.77× at T=4, up to **~4.1× by T=64**. It then **converges onto the
  exact same ~9.5e8 entries/s ceiling as `flat`** at T≥32 — i.e. once enough
  threads are thrown at it, the O(d) unranking arithmetic is *fully hidden*
  behind the same memory-bandwidth wall the trivial fill hits. This is the
  clean, quotable result: **zero-coordination parallel build scales until
  bandwidth, not the O(d) reconstruction cost, is the limit.**
- Efficiency collapses at high T for both variants simply because the
  workload is bandwidth-bound past ~T=32–64 on this 2-socket box (128 logical
  CPUs sharing a fixed DRAM bandwidth budget) — expected and not a flaw in the
  zero-coordination scheme.
- Peak RSS: 4.2–4.3 GB at N=1.12e9 (~3.8 bytes/entry, i.e. the 4-byte payload
  plus mmap/page-table overhead); 15.5 GB at N=4.08e9 (~3.8 bytes/entry, same
  ratio) — confirms the flat array carries **no** per-entry key overhead,
  unlike the hash baseline below.

### Baseline contrast: `std::unordered_map` insertion (same content, N=1.12e9)

Key = packed columns (variable bits/column so B up to ~46 fits, generalizing
bench.cpp's `pack()`); single shared map (1 thread) vs. the "fair best simple
parallel hash" — **16 shard maps, one mutex each, hash(key)-based sharding**,
built with 16 worker threads.

| build | threads | build_s | entries/s | peak RSS |
|---|---|---|---|---|
| `std::unordered_map`, 1 map | 1  | 234.27 | 4.79e6 | 43,096 MB |
| 16-shard `std::unordered_map` | 16 | 199.57 | 5.62e6 | 43,207 MB |

The map **comfortably fits in RAM at N=1e9** on this box (~43 GB, vs. ~415 GB
available) — unlike the 16 GB laptop the original spike used, where this size
would have been infeasible. We therefore did not need to fall back to
"largest N_hash that fits"; instead the RAM headroom lets the comparison run
at the paper's target N. (We did not extend the hash baseline to N=4e9 — at
~4× the memory, ~172 GB, and ~4× the single-threaded build time, ~15–16 min —
since the prompt's baseline-contrast requirement was specifically about the
N≈1e9 point; the flat array's own scaling to 4e9 is already covered above.)

**The headline comparison** (N=1.12e9): the tapered flat array builds in
**0.94–1.28 s** (any T, `flat` mode) or **1.17–4.85 s** (`unrank` mode,
depending on T) versus the hash's **234.3 s** (1 thread) / **199.6 s** (16
threads, 16 shards) — a **~183×–250× build-time advantage**, and 16-way
sharding only buys the hash a **1.17× speedup** (lock contention + pointer-
chasing node allocation dominate), vs. the flat array's zero-coordination
**~4×** (unrank mode) to memory-bandwidth-bound (flat mode) scaling. Memory:
flat array 4.2 GB vs. hash 43.1 GB at the same N — a **~10.2× memory
advantage**, even larger than the 5.7× measured at the smaller laptop scale
(RESULTS.md), consistent with the hash's fixed per-entry key+node overhead
dominating as N grows.

Raw CSV: `results_bigbox/task1_sweep.csv` (T-sweep); hash baseline numbers
above are from direct runs of `./build_parallel hash1 32 10 1 - 42` and
`./build_parallel hash16 32 10 16 - 42`.

## Task 2 — Extended in-RAM N-sweep (query latency + peak RSS)

Extended `bench.cpp`'s existing protocol (same `tapered`/`hash` modes; generalized
`pack()` from a hardcoded 4-bits/column, which only supports B≤16, to a
variable bit-width so bigger B values work — `rank_fixed()`/`value_of()`
untouched) to **M=10,000,000 queries**, seed=42, **fixed d=10**, sweeping B to
hit N≈{0.1e9, 0.5e9, 1e9, 2e9}: B∈{24, 29, 32, 34} → N = {92,561,040 /
472,733,756 / 1,121,099,408 / 1,917,334,783}. Checksums matched between
`tapered` and `hash` at every N (correctness).

**Variance check** (per the task's own 5% rule): the box was not fully idle —
the Task 3 `uv add torch ...` install was running concurrently in the
background for part of this sweep, and this is a shared multi-tenant machine.
Repeat runs showed **tapered-mode query latency varying ~20–30%** run-to-run
(e.g. N=1e9: 154.4/120.0/149.6 ns across 3 runs) — plausibly because a single
scheduling jitter/preemption has an outsized relative effect on a loop whose
total single-thread runtime is only ~1–2 seconds. **Hash-mode latency was far
more stable** (~5% spread, e.g. N=0.1e9: 110.8/112.0/116.5 ns), consistent with
its heavier per-query compute smoothing out brief jitter. Given this, tapered
numbers below are the **median of 3 runs**; hash numbers are **single runs**
(a hash rebuild at N=2e9 costs ~9 minutes, so 3× repeats were not run for
every cell — the one cell we did spot-check twice, N=0.1e9, showed hash's
low ~5% variance holds).

| N | tapered lat (median/3) | tapered peak RSS | hash lat | hash build_s | hash peak RSS |
|---|---|---|---|---|---|
| 92,561,040 (B=24)     | 98.9 ns  | 737 MB   | 110.8 ns | 17.6  | 4,189 MB  |
| 472,733,756 (B=29)    | 144.3 ns | 2,187 MB | 150.6 ns | 92.7  | 19,540 MB |
| 1,121,099,408 (B=32)  | 149.6 ns | 4,660 MB | 183.4 ns | 230.0 | 45,843 MB |
| 1,917,334,783 (B=34)  | 153.6 ns | 7,698 MB | 282.8 ns | 528.2 | 78,406 MB |

**Hash OOM threshold**: **the hash never OOM'd at any tested N up to 1.92e9**
on this 499 GB box (peak 78.4 GB at the largest N, comfortably under the
~415 GB available) — unlike the 16 GB laptop the original spike ran on, where
even the smallest of these sizes would already be tight. So there was no
"tapered only, hash can't build" regime to report here; instead the more
interesting server finding is in the **query-latency gap widening with N**:
tapered stays essentially flat (~99→154 ns across a 20× range of N — it's a
single O(d) address computation + one mostly-cache-resident-at-these-sizes
mmap read) while hash grows from 111→283 ns (~2.5×, more DRAM-resident
pointer-chases as the table grows past cache), so the **latency ratio widens
from ~1.1× to ~1.8×** as N grows — and the **memory ratio widens from 5.7× to
10.2×** (737MB→7.7GB tapered vs 4.2GB→78.4GB hash) over the same range. Both
of RESULTS.md's laptop-scale trends (latency ratio stabilizing ~1.3–1.8×,
memory advantage growing essentially unbounded with N) reproduce cleanly at
10–50× the laptop's N.

Raw log: `results_bigbox/task2_sweep.log`.

## Task 3 — GPU: α-entmax application dataset + exact-solver baseline timing

PyTorch+CUDA **was** available (`uv add torch transformers entmax datasets numpy`
resolved `torch==2.13.0+cu130` automatically against the driver's CUDA 13.1,
detected `NVIDIA H200 NVL`), so this ran in full, not skipped.

### Dataset dump (`task3_dump_entmax_rows.py`)

- GPT-2 small (12 layer, 12 head, n_embd=768, head_dim=64) loaded via
  `transformers`, `attn_implementation="eager"`, run on the H200.
- **Text source deviated from plan**: `datasets.load_dataset("wikitext",
  "wikitext-2-raw-v1")` failed under this environment's `datasets==5.0.0`
  (`HfUriError` — the loader's URI parsing changed and rejected the plain
  `wikitext` repo id). Fell back to this repo's own markdown docs
  (`PAPER_PLAN.md`, `RESULTS.md`, `THEORY.md`, `THEORY_PENALTY.md`, repeated
  20×) as the "any freely available text" fallback the prompt allowed — still
  natural English text, just not wikitext specifically. Tokenized to
  **50,176 tokens** (98 sequences × 512 tokens, GPT-2 BPE).
- **Method**: rather than trust `output_attentions` (which returns
  POST-softmax weights), hooked each layer's `c_attn` (QKV projection),
  recomputed `q @ k^T / sqrt(head_dim)` ourselves, and applied the causal
  restriction by slicing — this is the true pre-softmax score, independent of
  whatever attention kernel (eager/SDPA/flash) the rest of the model uses.
- Sampled ~2.77% of all (layer, head, position) rows (skipping position 0,
  which is a trivial length-1 row) to hit the ~200k target: **199,669 rows**
  dumped, each as (top-32 sorted scores fp32, true causal row length).
  94.2% of rows have causal length ≥32 (expected — most query positions in a
  512-token sequence see ≥32 valid keys; this is just the row's raw causal
  length, not a statement about entmax's active-support size, see below).
- Saved `results_bigbox/entmax_rows.npz`, **23.3 MB** (well under the ~1GB cap).

### Exact solver baselines (`task3_entmax_solver_bench.py`)

All CPU, single thread (`torch.set_num_threads(1)`, `OMP/MKL/OPENBLAS_NUM_THREADS=1`),
α=1.5, on 100,000 rows sampled from the dumped dataset:

| method | μs/row (batched, vectorized) |
|---|---|
| our numpy **bisection** (50 iters, batched across rows) | 8.770 |
| our numpy **exact sorted-support** (closed-form mean/variance trick, entmax-package-style) | 1.013 |
| **torch `entmax15`** (the actual `entmax` package), CPU | 0.286 |

**Important caveat on what "μs/row" means here**: the numbers above are a
single vectorized call over all 100k rows at once, divided by row count —
standard practice for reporting these solvers' throughput, and what makes
`entmax15`'s C++/vectorized internals look so fast. But if the real serving
pattern is **one row at a time** (e.g. autoregressive decode, one query
position per step), per-call Python/tensor overhead dominates and the number
is very different — we spot-checked this for `entmax15`:

| method | μs/row (single-row loop, realistic per-query serving) |
|---|---|
| torch `entmax15`, one row at a time (n=20,000) | **37.570** |

That's **~130× slower** than the batched number. **This is the baseline the
memo table on the laptop should actually target if it's answering one query
at a time** (which is the paper's use case — a single attention row's τ per
lookup); the 0.286 μs/row batched figure is the best case only if whole
attention score matrices are processed in bulk (e.g. during prefill). Both
numbers are recorded so the laptop benchmark can pick the fair comparison.

**Support-saturation check**: only **0.91%** of the 100k benchmarked rows had
their α-entmax active support reach the full top-32 window (i.e. saturate our
truncation) — confirms the paper's assumed "exact tail" property (PAPER_PLAN.md's
"entries below τ have exactly zero influence") holds in practice for GPT-2
attention at α=1.5; top-32 storage would only truncate the true support for
~1 in 110 rows.

**Sanity check** (1000 random rows, comparing against the actual `entmax`
package's `entmax15` output in float64): max abs diff over resulting
probabilities — **bisection: 1.05e-15**, **exact sorted-support: 9.21e-15**.
Both many orders of magnitude under the requested 1e-6 threshold.

Raw files: `results_bigbox/entmax_rows.npz` (dataset, goes back to the
laptop), `results_bigbox/task3_solver_timings.json` (all timings above).

Note: Task 1 arrays are stored in `/dev/shm` (tmpfs, RAM-backed) rather than
on the NVMe filesystem: at N=4e9 the flat array is only 16 GB (fits trivially
in RAM), and tmpfs removes any ambiguity from the underlying disk's own
caching behavior when comparing thread-scaling — we want to measure pure
compute/memory-bandwidth scaling of the fill loop, not filesystem I/O.
(Sanity check: single-threaded `dd` O_DIRECT write to the NVMe-backed home
filesystem measured ~1.6 GB/s — plenty fast for a one-shot 16 GB build, but a
poor match for isolating a 1→128-thread compute-scaling sweep, since at high
T the disk itself would become the bottleneck rather than the thing under
test.)

---

## Follow-up addendum (priority change + Task 4)

### Task 3 status confirmation (priority change: entmax is now the primary application)

**Task 3 was never skipped** — PyTorch+CUDA was available on this box (see
above: `torch==2.13.0+cu130`, H200 detected), so it ran end-to-end on GPU as
originally reported. Given `entmax_rows.npz` is now the single most important
deliverable, we re-audited it (not regenerated — nothing was wrong) before
sign-off:

- Shapes/dtypes: `top32` (199,669, 32) float32, `row_len`/`layer`/`head`
  (199,669,) int32/int16/int16 — all as documented.
- **No NaNs, no +inf anywhere.** -inf appears only as intentional padding for
  rows with true causal length <32 (2.79% of all entries across the dataset).
- **Sort order verified**: spot-checked every 500th row (400 rows) — every
  row's finite entries are strictly non-increasing (descending), 0 failures.
- **row_len consistency verified**: for every spot-checked row, the count of
  finite entries in `top32` exactly equals `min(row_len, 32)`, 0 mismatches.
- `row_len` ranges 2–512 (min 2 is expected: position-0 rows were
  deliberately excluded as trivial length-1), mean 257.4. `layer`∈[0,11],
  `head`∈[0,11] — full GPT-2-small coverage as intended.

No changes made to the file; it is confirmed sound as delivered. The one
caveat already on record still stands: the underlying text is this repo's own
markdown docs (repeated 20×), not wikitext, because `datasets==5.0.0`'s loader
rejected the `wikitext` repo id in this environment — still natural English
text, satisfying the prompt's "any freely available text" fallback, but if
the paper wants wikitext specifically, that call site needs revisiting on
whatever machine ends up producing the final dataset.

### Task 4 — cold-cache NVMe query latency (**completed**, run by the user)

I (the assistant) could not run this myself — no way to supply a sudo
password in this non-interactive session — so the user ran it directly on
the console, with me guiding the protocol.

**The system-wide `drop_caches` mechanism is blocked entirely on this box,
even for root**: `sync && echo 3 | sudo tee /proc/sys/vm/drop_caches` failed
with `tee: /proc/sys/vm/drop_caches: Read-only file system` *after* the sudo
password was accepted — i.e. this isn't a permissions gap. **This machine is
a Docker container**, and Docker mounts `/proc/sys` read-only by default (a
container isn't supposed to mutate host kernel parameters) — so `drop_caches`
is unreachable here regardless of in-container privilege; it would need
`--privileged` or a specific bind-mount at container-creation time, neither
of which applies after the fact from inside.

That turned out not to matter. `ooc_bench.cpp`'s own `build()` already calls
`posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` right before closing the file —
a per-file cache-eviction request that needs no privilege at all — so the
protocol run was, in the end:
```
g++ -O3 -march=native -o ooc_bench ooc_bench.cpp
./ooc_bench build 32 10          # N=1,121,099,408, ~4.48 GB file, NVMe-backed home fs
                                  #   (fdatasync + posix_fadvise(DONTNEED) at the end of build())
./ooc_bench query 32 10 20000 42 # 20k random point queries, mmap + MADV_RANDOM
```

**Cold run** (immediately after build, no other process having touched the file):
```
M=20000  total=1.4s  mean=71us  p50=67us p90=82us p99=85us
cache-hit(<100us)=99.9%  disk(>=100us)=0.1%  disk-mean=0.1ms
checksum=9995.744126 expect=9995.744126  CORRECT
```

**One caveat on the tool's own classifier**: `ooc_bench.cpp`'s "cache-hit if
latency <100µs" heuristic was written with an HDD contrast in mind (disk
seeks ~8ms — trivially separable from a cache hit at any threshold under a
few ms). On NVMe, genuine cold disk reads *also* complete in the tens-of-
microseconds range, so that heuristic cannot actually distinguish "page-cache
hit" from "real, fast NVMe read" here — its "99.9% cache-hit" line should be
read as "99.9% of queries completed in <100µs," not "99.9% were served from
RAM." The real evidence for what these queries were is the confirmatory
control below.

**Confirmatory control — immediate re-run, same file, no rebuild:**
```
M=20000  total=0.0s  mean=1us  p50=1us p90=1us p99=2us
cache-hit(<100us)=100.0%  disk(>=100us)=0.0%  disk-mean=0.1ms
checksum=9995.744126 expect=9995.744126  CORRECT
```
Latency collapses **~70×** (mean 71µs → 1µs) the moment the same pages are
queried a second time — proof the first run's pages were genuinely evicted
(cold) and the second run found them page-cache-resident (warm). This
confirms `posix_fadvise(DONTNEED)` alone was sufficient; the blocked
system-wide `drop_caches` was never actually needed.

**The headline contrast** (this is the number the paper wants): vs. the 16GB
laptop's HDD numbers for the same protocol at N=5.57e9 (p50 15.5 ms,
disk-mean 18.9 ms, one seek/query) —

| | HDD (laptop, N=5.57e9) | NVMe (this box, N=1.12e9), cold |
|---|---|---|
| p50 | 15.5 ms | **0.067 ms** (67 µs) |
| mean / disk-mean | 18.9 ms | **0.071 ms** (71 µs) |
| **speedup** | — | **~231× (p50), ~266× (mean)** |

(N differs 5× between the two runs, but both regimes are already deep
out-of-core relative to their own RAM — 22.3GB file vs. 16GB laptop RAM;
4.48GB file vs. NOT RAM-resident once evicted, on this run — so the
per-query cost here is fairly attributed to media latency, not a cache-size
artifact.) No mechanical seek exists on NVMe; the ~70µs is queue-depth-1
flash-access + controller/PCIe overhead, vs. the HDD's dominant seek time.

**A clean three-tier latency hierarchy falls out of combining this with
Tasks 1–2**, all at comparable N (~1e9–2e9):
1. **tmpfs-backed, already mapped in the same process** (Task 1/2's
   `bench.cpp`/`build_parallel`, `/dev/shm`): **~100–280 ns**.
2. **Disk-backed file, page-cache-resident, fresh `mmap` (minor fault only)**
   (this task's warm control): **~1 µs** — ~7× slower than tier 1, the cost
   of a minor page fault establishing a new page-table entry even though no
   I/O occurs.
3. **Disk-backed file, evicted, genuine NVMe read** (this task's cold run):
   **~67–85 µs** — ~70× slower than tier 2, ~500× slower than tier 1.

Each tier is real and paper-relevant: tier 1 is what the tapered index gets
"for free" once resident (the common case after warmup); tier 3 is the
worst-case first-touch cost the paper's out-of-core framing is about, and is
still ~230× cheaper than the same worst case on HDD.

Raw session transcript (build/cold-query/warm-query) preserved above; no
additional files needed since `ooc_bench.cpp`'s own stdout already contains
every number reported here.
