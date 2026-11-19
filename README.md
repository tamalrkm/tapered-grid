# tapered-grid: memo tables without keys

A header-only C++17 memo table for functions of a sorted argument. It stores
values only: the address of an entry is the lexicographic rank of its sorted
argument, computed in O(d) from one shared Pascal table. This repository is the
artifact of *Memoization Without Keys: Compact, Out-of-Core Tables for Functions
of Sorted Arguments* (Software: Practice and Experience, under review). It holds
the library, its tests, every benchmark, the raw logs behind every measured
number in the paper, and the scripts that turn those logs into the paper's
numbers, tables and figures.

## Check it

```bash
make verify          # C++ unit tests, Listing 1 of the paper, Python reference checks (about 2 min)
make check-numbers   # regenerate every number and table from results/ and diff against paper/
```

`make verify` needs a C++17 compiler and Python 3; `make check-numbers` needs Python 3
only. Neither needs NumPy or any other package. What `make verify` covers:

| Check | Where |
|---|---|
| rank, unrank and the O(1) neighbour step against brute force, every key of every grid with B ≤ 8, d ≤ 6 (6,426 keys); prefix blocks contiguous with the stated length | `tests/test_tg.cpp` |
| the one-read rank equals the two-read form, exhaustively and on random keys of large shapes | `tests/test_tg.cpp` |
| Eq. (3) of Biswas and Regan (2015) agrees on all 2,989 pinned keys and collides off that domain (B = d = 2 counterexample) | `tests/test_tg.cpp` |
| simplex interpolation exact on affine f; every vertex a stored key | `tests/test_tg.cpp` |
| file round trip, payload checksum, corrupted header and wrong type rejected | `tests/test_tg.cpp` |
| parallel fill equals sequential fill; `get_many` equals single lookups | `tests/test_tg.cpp` |
| the code printed as Listing 1 compiles and runs | `tests/listing1.cpp` |
| the published worked examples (indices 328 and 400), nested ranker on 6,501 keys and 30 random configurations | `src/refimpl.py`, `src/warped.py` |
| the taper-mismatch penalty formula against the exact discrete optimum, 33 cells | `src/verify_penalty.py` |

Two lines in the output of `warped.py`, marked `[A]`, say that the literal reading
of Eq. (7) of the 2015 paper does not give its own index 400. That is expected; the
`[B]` lines, the nested ranker, must pass.

## Use it

Copy `src/tg.hpp`. Listing 1 of the paper is `tests/listing1.cpp`:

```cpp
auto t = tg::Table<float>::create("root.tg", sh, meta);  // build once
t.fill([&](const int* m, int d) { return f(m, d); }, 4); // 4 threads, no locks
t.sync();                                                // flush, record the checksum

auto memo = tg::Table<float>::open("root.tg");           // map read-only, header validated
float v = memo.at(tg::rank(m, sh));                      // one memory access
memo.get_many(keys, n, out);                             // n independent lookups, overlapped
```

`src/tg.hpp` provides `Shape`, `rank`, `unrank`, `step`, `next`, `prefix_block`,
`Spacing`, `snap_nearest`, `locate`, `interp_simplex`, `check_key` and `Table<T>`
(anonymous memory, or a file with a 4 KiB header page: magic, version, endianness
tag, element type and size, shape, payload checksum). Limit: `B + d ≤ 68`, so that
all binomials fit in 64 bits. Linux; no dependencies beyond POSIX `mmap`.

## From logs to paper

Nothing in the paper's tables or measured prose is typed by hand.
`analysis/make_numbers.py` reads `results/<machine>/*.jsonl` and writes
`paper/numbers.tex` (one `\def` per quoted number) and `paper/tables/*.tex`
(table bodies). It stops with an error if any two structures disagree on the
answers to a query stream, if a parallel build reports a verification error, or if
an array's contents differ across thread counts. `analysis/make_figs.py` writes
`paper/figs/*.pdf` and needs matplotlib.

```bash
make numbers   # paper/numbers.tex, paper/tables/
make figures   # paper/figs/
```

| In the paper | Plan (`scripts/run.py`) | Binary | Raw log |
|---|---|---|---|
| Table 3, headline factors | all of the below | | `results/` |
| Table 4, head-to-head | `h2h` | `bench` | `results/{server,desktop}/h2h.jsonl` |
| Table 5, overlap against d and batch size | `window` | `bench` | `results/{server,desktop}/window.jsonl` |
| Table 6, Figure 1, scaling | `sweep` | `bench` | `results/{server,desktop}/sweep.jsonl` |
| Table 7, construction; expensive payload | `parallel` | `build_parallel` | `results/server/parallel.jsonl` |
| Table 8, Figure 2, out of core | `ooc` | `ooc_bench`, `pl_fit` (trace) | `results/{server,desktop}/ooc.jsonl` |
| Table 9, vector-root microbenchmark, bit identity | `pl` | `pl_bench` | `results/{server,desktop}/pl.jsonl` |
| Table 10, end-to-end fit | `plfit` | `pl_fit` | `results/{server,desktop}/plfit.jsonl` |
| α-entmax negative result | `entmax` | `entmax_bench` | `results/server/entmax.jsonl` |
| the default page policy, in the prose of Section 6.2 | `pages` | `bench` | `results/{server,desktop}/pages.jsonl` |
| instructions per coordinate of the rank loops (the paper quotes the desktop build; the server's GCC 13.3 compiled the one-read loop into a longer body, 64 instructions and 18 reads per pass, presumably several coordinates at a time) | | `analysis/loopstats.py` | `results/<machine>/loopstats.json` |
| the worked example of Section 4 | | `analysis/taper_example.py` | `results/desktop/taper.json` |
| the query stream's working set (hottest 32 MiB), Section 6.2 | | `analysis/query_stream.py` (numpy) | `results/query_stream.json` |
| other users' load on the shared server, Section 5 | | `results/server/other_load_sampler.py` (ran beside the runner, not part of it) | `results/server/other_load.tsv`, `other_load_spans.tsv` |
| Table 2 and the penalty check of Appendix B | | `src/verify_schedule.py` (Table 2 is the last block it prints), `src/verify_penalty.py` | printed |

Every row of a log records the exact command, the repetition, the load average
before and after, and the available memory. `results/<machine>/env-*.json` records
`lscpu`, `free`, the kernel, the compiler, the flags, the transparent huge page
setting and the library versions.

## Measure again

```bash
scripts/get_deps.sh            # abseil from source with our flags, Boost 1.86 headers (needs network)
make -j && make modes          # must print "absl available" and "boost available"
python3 scripts/run.py --machine server  --plans all                          # about 7 h
python3 scripts/run.py --machine desktop --plans all --ooc-dir /data/tg-ooc   # about 3 h
```

Re-running a command resumes: measurements already in `results/` are skipped. Each
configuration runs as its own process; a repetition runs every configuration once,
in a shuffled order, before the next begins. Single-threaded cells are pinned to one
core, and every process runs with `GLIBC_TUNABLES=glibc.malloc.hugetlb=1`, so that
absl, Boost and `std::unordered_map` get huge pages as the array does. In the code,
`pl` stands for the vector root of Biswas and Regan, for historical reasons. The out-of-core experiments only mean something when RAM is smaller than the
22.3 GB table file, and `--ooc-dir` must be on the disk under test. The runner
refuses to start them below 12 GiB of available memory unless given `--force`,
because a machine with too little page cache gives a different experiment.

The α-entmax rows are not redistributed, because they derive from GPT-2 and
WikiText-2. `python3 src/dump_entmax_rows.py` regenerates them in about 15 minutes on
a CPU (needs torch, transformers, datasets) with seed 0, as `data/entmax_u.bin`.

`paper/main.tex` is the manuscript source as of this release. An archive cannot cite its
own version DOI, so this copy cites the DOI of all versions; the submitted manuscript
cites the version DOI and is otherwise the same.

## Citing

Cite the Zenodo record: <https://doi.org/10.5281/zenodo.21675851> (all versions).
MIT licence. The design follows Biswas and Regan, *Approximation of function
evaluation over sequence arguments via specialized data structures*, Theoretical
Computer Science 607 (2015) 113–123.
