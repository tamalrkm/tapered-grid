# Change log — tapered-grid paper (TCS submission)

Files revised: `main.tex`, `refs.bib`, `main.bbl` (regenerated).
Build verified: `pdflatex → bibtex → pdflatex → pdflatex`, 24 pages,
**0 errors, 0 undefined references, 0 undefined citations, 0 PDF-bookmark
warnings, 0 overfull boxes above 4.4 pt**. The XeLaTeX build, which
previously died on line 1, now also completes with 0 errors.

The `.bbl` was regenerated with the genuine `elsarticle-num.bst`. I first
confirmed that regenerating from your *unmodified* `refs.bib` reproduced your
uploaded `.bbl` byte-for-byte, so the new one differs only where the `.bib`
was actually edited.

---

## 1. Build-breaking and structural bugs

| # | Issue | Fix |
|---|-------|-----|
| 1 | `\pdfoutput=1` (bare) — XeTeX has no such primitive, so the XeLaTeX build failed immediately, contradicting the header comment claiming it builds under XeLaTeX. | Wrapped as `\ifx\pdfoutput\undefined\else\pdfoutput=1\fi`. Comment updated to explain why the guard is needed. |
| 2 | **Table 4** (`tab:parallel`): `\begin{tabular}{lcccc}` declared 5 columns but every row had 4 cells, leaving a stray empty column with booktabs rules overhanging into it. | Changed to `{lccc}`. All six tabulars now pass an automated spec-vs-row column check. |
| 3 | **No bias lemma existed.** The text referred to "the bias lemma" four times, Appendix B was titled *Proof of the bias lemma*, and `\newtheorem{lemma}` was declared — but there was no `\begin{lemma}` anywhere. | The §5 *Bias* result is now **Lemma 3 (Rounding bias)**, formally stated with its hypotheses ($f \in C^2$, $C^1$ query density, nearest-gridpoint rounding). All four prose references replaced with `Lemma~\ref{lem:bias}`; Appendix B retitled *Proof of Lemma 3*. |
| 4 | Orphaned `proof` environment after Eq. (1) — a proof attached to no formal statement. | Eq. (1) is now **Proposition 1 (Table-free rank)**, with the proof following it. Cross-referenced from §3.3. |

Statement numbering now runs: Proposition 1 (§3.1) → Theorem 2 (§5) → Lemma 3 (§5). All `\ref`s resolve.

## 2. Numbers that did not reconcile

| # | Was | Now | Reasoning |
|---|-----|-----|-----------|
| 5 | Plackett–Luce speedup "26–55×" (5 places: abstract, contribution 5, §6.6, §7.1, conclusion) | **25–55×** | Table 5's bottom row ($B{=}16,d{=}16$) is 25×. The stated lower bound excluded a row of the author's own table. |
| 6 | "183–250× build-time advantage" from "0.9–4.9 s" vs 234 s | **48–250×**, with the three build times spelled out: 0.9 s (`flat`, $T{=}1$), 4.9 s (`unrank`, $T{=}1$), 1.2 s ($T{=}64$) | 234/4.9 = 48, 234/0.9 = 250. The old range was not reconstructible from any pair of stated numbers. |
| 7 | Hash peak RSS at $N{=}1.12$B given as **45.8 GB** (Table 3) and **43.1 GB** (§6.5); memory ratio given as 10.2× in §6.5 but 9.8× from Table 3 at that $N$ | Both numbers retained, now explicitly reconciled: §6.5 states they are build-only peaks, cites Table 3's build-plus-query peaks (4.66 / 45.8 GB → 9.8×) and notes the headline 10.2× is the $N{=}1.92$B row | See **verification needed**, item A. |
| 8 | Abstract: "1.3–1.8× faster" | **1.1–1.8×** | Contribution 3 and §6.3 both say 1.1–1.8×; no table produces a 1.3. |
| 9 | "$N{=}1.92$B (20× the laptop spike)" | "$51\times$ the largest in-RAM configuration of Table 2" | 1.92B / 37.44M = 51.3. No quantity in the paper was 20× smaller than 1.92B. |
| 10 | "On 128 threads a zero-coordination build reaches the memory-bandwidth ceiling" (contribution 2) and "built by 128 threads" (conclusion) | **64 threads** in both | Table 4 stops at $T{=}64$. The machine description ("128-thread dual-EPYC server") in §6 is untouched and remains correct. |
| 11 | "measured bias 0.30–0.47× the mean absolute error" (§6.6, §7.1) | Same range, now qualified "**on the three finer grids**", with §7.1 naming the excluded case ($0.99\times$ at $B{=}12,d{=}8$) | The range silently dropped a row of Table 5. A referee would have caught this. |
| 12 | §5 said the closed form is within "0.6–8%"; Appendix A.5 said "0.6–8.2%" | **0.6–8.2%** in both | |

Independently re-derived and **confirmed correct** (no change needed):
C(29,14) = 77,558,760 · C(36,15) = 5,567,902,560 · C(41,10) ≈ 1.121 B ·
C(46,10) ≈ 4.076 B · C(19,3) = 969 · C(17,2) = 136 · $B{=}16,d{=}13 \to$ 37,442,160 ·
the 6,426 exhaustive key count over $B\le8, d\le6$ · 1675/294 = 5.7× ·
78.4/7.70 = 10.2× · 22.27 GB = 1.4× of 16 GB · 5.5679e9 × 43.5 B = 242 GB ·
all four Table 4 speedup ratios · all Table 5 memo latencies against 3.8 µs / 28 µs.

## 3. Typesetting and journal-compliance

13. **All five uncited tables now cross-referenced in the running text**
    (`tab:complexity`, `tab:headtohead`, `tab:scaling`, `tab:parallel`,
    `tab:penaltyverify`). Elsevier production flags floats that are never
    called out.
14. **Overfull boxes.** The worst was 43.5 pt (~1.5 cm into the margin) at the
    $\mathcal{M}(B,d)$ display in §2 — the display is now split, with the
    cardinality moved into the following sentence. Four more in the 19–30 pt
    range were caused by unbreakable tokens (`exponents---together`,
    `otherwise---is`, `bit-allocation/reverse`, a `\paragraph` head running
    straight into `$\alpha$-entmax`) and were rephrased. Nothing above 4.4 pt
    remains.
15. **`microtype` added** as `\usepackage[protrusion=true,expansion=false]{microtype}`.
    `expansion=false` is deliberate: font expansion needs scalable Type1/OpenType
    fonts and hard-errors on bitmap PK fonts, which would break the build on a
    TeX installation lacking `cm-super`. Protrusion alone is safe everywhere
    and does the work here.
16. **PDF bookmarks.** Eight `hyperref` "Token not allowed in a PDF string"
    warnings produced mangled bookmarks. Six headings containing math are now
    wrapped in `\texorpdfstring{...}{...}` (the two *Geometric schedule* appendix
    subsections, *Out of core*, *α-entmax thresholds*, *Scaling in N*,
    *Derivation of Theorem 2*, *Proof of Lemma 3*). Zero warnings remain.
17. **Stray Unicode em dash** in the Algorithm 1 caption replaced with `---`.
    The body is now pure ASCII apart from a comment on line 1.
18. **Draft macros `\TODO` and `\PLH` removed** from the preamble.

## 4. Bibliography

19. `novak2008`: `year={2008}, note={2008/2010/2012}` printed as
    "…, 2008, 2008/2010/2012." → now `year={2008--2012}`, printing cleanly.
20. `nguyen2024` → **`nguyen2025`**: the entry is ICLR **2025**. The single
    `\cite` in §8 updated to match.

`\address` was **kept** rather than switched to `\affiliation`. Newer
`elsarticle` prefers `\affiliation`, but `\address` works in every version and
switching risks breaking your local build for no visible gain. Worth revisiting
only if Elsevier's editorial system objects.

---

## Verification needed from you

These require your raw measurement logs; I did not alter any measured value.

- **A. Hash peak RSS at $N{=}1.12$B, $d{=}10$: 45.8 GB or 43.1 GB?** I have
  written §6.5 on the assumption that 43.1 GB is a build-only peak and Table 3's
  45.8 GB is the build-plus-query peak, which is physically plausible and makes
  both self-consistent (43.1 GB pairs with the 3.8 B/entry figure; 4.2 GB × 10.2
  ≈ 43.1). **If that assumption is wrong, this passage needs rewording.** The
  43.5 B/entry in §6.4 is fine either way — that is a $d{=}15$ configuration, so
  a larger per-entry key is expected.
- **B. The 1.2 s figure at $T{=}64$** I derived as $1.12\text{e}9 / 9.59\text{e}8$
  from Table 4. Confirm against your logs.
- **C. Table 5, row 1 rounding.** $3800/70 = 54.3$, printed as 55×; $3800/149 = 25.5$,
  printed as 25×. Consistent only if the true exact-solve time is slightly above
  3.8 µs. Harmless, but a careful referee may ask.
- **D. Affiliation line** reads "Department of Computer Science, RKMVERI" —
  please confirm that is the intended department.
- **E. §6.1** claims exhaustive verification over "every grid with $B\le8$,
  $d\le6$: 6,426 keys". I verified the arithmetic ($\sum_{B\le8,d\le6}\binom{B+d-1}{d} = 6426$
  exactly), so the count is right — no action, noted only because it is the kind
  of number referees recompute.

## Rebuilding

```bash
rm -f main.aux main.bbl main.blg main.log main.out
pdflatex main && bibtex main && pdflatex main && pdflatex main
```

Delete the auxiliaries first — a stale `.bbl` or `.aux` is the usual silent
failure here, and the numbering of Proposition 1 / Theorem 2 / Lemma 3 will not
settle until the third pass.
