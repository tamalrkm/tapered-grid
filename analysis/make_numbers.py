#!/usr/bin/env python3
"""Turn raw JSON results into the paper's numbers and table bodies.

    python3 analysis/make_numbers.py --results <dir containing results/> [...] --paper <paper dir>

Writes <paper>/numbers.tex (every headline macro used in main.tex) and
<paper>/tables/*.tex (table bodies \\input by main.tex). Anything not yet
measured is emitted as \\pend{--}. Standard library only.

Conventions: a cell is the median over runs; ratios are ratios of medians;
"spread" is the interquartile range relative to the median, and each table
caption quotes the largest such spread among its cells. Memory is peak RSS in
MiB minus the query buffer the process allocated, unless a column says
"structure".
"""
import argparse, json, os, statistics as st, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from load import rows, summ, group  # noqa: E402

PEND = r"\pend{--}"


def f(x, nd=1):
    if x is None:
        return PEND
    return f"{x:.{nd}f}"


def fx(x, nd=1):
    return PEND if x is None else f"{x:.{nd}f}"


class Spread:
    """Tracks the largest relative IQR among the cells of one table."""
    def __init__(self):
        self.worst = 0.0

    def see(self, s):
        if s and s["med"]:
            self.worst = max(self.worst, (s["q3"] - s["q1"]) / abs(s["med"]))
        return s["med"] if s else None

    def pct(self):
        return f"{100 * self.worst:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True, nargs="+", help="directories holding results/<machine>/*.jsonl")
    ap.add_argument("--paper", required=True)
    a = ap.parse_args()
    R = a.results
    def rowsR(machine, plan):
        return [r for root in R for r in rows(root, machine, plan)]
    os.makedirs(os.path.join(a.paper, "tables"), exist_ok=True)
    M = {}                                     # macro name -> value string

    # ---------------- head-to-head ----------------------------------------
    hs, hd = rowsR("server", "h2h"), rowsR("desktop", "h2h")
    check_answers(hs + hd + [r for m_ in ("server", "desktop") for p_ in ("sweep", "window") for r in rowsR(m_, p_)])
    def g(rs, val):
        return group(rs, lambda r: r["mode"], val)
    qs, qd = g(hs, "query_ns"), g(hd, "query_ns")
    bs = g(hs, "build_s")
    ms = g(hs, "peak_rss_mib")
    qbuf = (hs or hd)[0]["qbuf_mib"] if (hs or hd) else 148.8
    M["hQbuf"] = f(qbuf, 0)
    sp = Spread()
    def med(d, k):
        return sp.see(d.get(k))
    # label, base mode, has dependent column, has batched column, has memory and build columns
    H = [("index arithmetic alone", "rankonly", 0, 0, 0),
         ("same, two-read loop", "rankonly+two", 0, 0, 0),
         ("reads alone, addresses known", "tapered-ram+pre", 1, 0, 0),
         ("array, anonymous memory", "tapered-ram", 1, 1, 1),
         ("array, two-read loop", "tapered-ram+two", 1, 1, 0),
         ("array, file mapping", "tapered", 1, 1, 1),
         ("array, file mapping, pinned queries", "tapered+pin", 1, 0, 0),
         ("2015 node-count index, pinned", "tabled", 1, 0, 0),
         (r"\absl{}", "absl", 1, 1, 1),
         (r"\bflat{}", "boost", 1, 0, 1),
         ("open addressing (hand-written)", "oahash", 1, 1, 1),
         (r"\umap{}", "hash", 1, 0, 1)]
    lines = []
    for label, m, dep, batch, mem in H:
        cells = []
        for d_ in (qs, qd):
            cells += [f(med(d_, m)), f(med(d_, m + "+dep")) if dep else "---", f(med(d_, m + "+batch")) if batch else "---"]
        rss = (ms[m]["med"] - qbuf) if mem and m in ms else None
        b_ = bs.get(m)
        tail = (f"{f(rss, 0)} & {f(b_['med'] if b_ else None, 2)}") if mem else "--- & ---"
        lines.append(f"{label} & " + " & ".join(cells) + f" & {tail} \\\\")
    open(os.path.join(a.paper, "tables", "headtohead.tex"), "w").write("\n".join(lines) + "\n")
    M["hSpreadHH"] = sp.pct()
    def iqr(d, keys):
        """largest relative IQR among the cells a factor is computed from"""
        sp_ = Spread()
        for k in keys:
            sp_.see(d.get(k))
        return sp_.pct()

    def ratio(d, x, y):
        return d[x]["med"] / d[y]["med"] if x in d and y in d else None
    FLAT = ("absl", "oahash", "boost")
    def bestflat(d, sfx=""):
        c = [m for m in FLAT if m + sfx in d]
        return min(c, key=lambda m: d[m + sfx]["med"]) if c else None
    fname = {"absl": r"\texttt{absl}", "oahash": "the hand-written table", "boost": "Boost's table"}
    for tag, d_ in (("S", qs), ("D", qd)):
        bi, bd_, bb = bestflat(d_), bestflat(d_, "+dep"), bestflat(d_, "+batch")
        if bi:
            M[f"hInd{tag}"] = f(ratio(d_, bi, "tapered-ram"), 2); M[f"hIndName{tag}"] = fname[bi]
            M[f"hIndBest{tag}"] = f(d_[bi]["med"], 1)
            M[f"hTwoInd{tag}"] = f(ratio(d_, bi, "tapered-ram+two"), 2)
            M[f"hIqrInd{tag}"] = iqr(d_, ["tapered-ram", bi])
        if bd_:
            M[f"hDep{tag}"] = f(ratio(d_, bd_ + "+dep", "tapered-ram+dep"), 2); M[f"hDepName{tag}"] = fname[bd_]
            M[f"hIqrDep{tag}"] = iqr(d_, ["tapered-ram+dep", bd_ + "+dep"])
        if bb:
            M[f"hBatch{tag}"] = f(ratio(d_, bb + "+batch", "tapered-ram+batch"), 2); M[f"hBatchName{tag}"] = fname[bb]
            M[f"hIqrBatch{tag}"] = iqr(d_, ["tapered-ram+batch", bb + "+batch"])
        if "tapered-ram+two" in d_ and "tapered-ram" in d_:
            M[f"hTwoGain{tag}"] = f(ratio(d_, "tapered-ram+two", "tapered-ram"), 1)
            M[f"hIqrTwo{tag}"] = iqr(d_, ["tapered-ram+two", "tapered-ram"])
            M[f"hBatchGain{tag}"] = f(ratio(d_, "tapered-ram", "tapered-ram+batch"), 1)
            M[f"hMlpOne{tag}"] = f(ratio(d_, "tapered-ram+dep", "tapered-ram"), 1)
            M[f"hMlpTwo{tag}"] = f(ratio(d_, "tapered-ram+two+dep", "tapered-ram+two"), 1)
            M[f"hMlpPre{tag}"] = f(ratio(d_, "tapered-ram+pre+dep", "tapered-ram+pre"), 0)
    M["hIdxNs"] = f(qs["rankonly"]["med"], 0) if "rankonly" in qs else PEND
    M["hQryUmap"] = f(ratio(qs, "hash", "tapered-ram"), 1) if "hash" in qs else PEND
    # build, single thread, vs flat tables (server h2h)
    bf = [bs[m]["med"] / bs["tapered-ram"]["med"] for m in FLAT if m in bs and "tapered-ram" in bs]
    M["hBuildFlatLo"], M["hBuildFlatHi"] = (f(min(bf), 0), f(max(bf), 0)) if bf else (PEND, PEND)
    if bf: M["hIqrBuild"] = iqr(bs, ["tapered-ram", *FLAT])
    bd_ = g(hd, "build_s")
    bfd = [bd_[m]["med"] / bd_["tapered-ram"]["med"] for m in FLAT if m in bd_ and "tapered-ram" in bd_]
    if bfd:
        M["hBuildFlatLoD"], M["hBuildFlatHiD"] = f(min(bfd), 0), f(max(bfd), 0); M["hIqrBuildD"] = iqr(bd_, ["tapered-ram", *FLAT])
    if bf and bfd:
        M["hBuildAllLo"], M["hBuildAllHi"] = f(min(bf + bfd), 0), f(max(bf + bfd), 0)
    fc = [qs["tapered" + s_]["med"] - qs["tapered-ram" + s_]["med"] for s_ in ("", "+dep", "+batch")
          if "tapered" + s_ in qs and "tapered-ram" + s_ in qs]
    if fc: M["hFileCostLo"], M["hFileCostHi"] = f(min(fc), 0), f(max(fc), 0)
    for tag, d_ in (("S", qs), ("D", qd)):         # what the two-read loop added, in ns
        if "rankonly+two" in d_ and "rankonly" in d_ and "tapered-ram+two" in d_:
            M[f"hRankAdd{tag}"] = f(d_["rankonly+two"]["med"] - d_["rankonly"]["med"], 0)
            M[f"hLookAdd{tag}"] = f(d_["tapered-ram+two"]["med"] - d_["tapered-ram"]["med"], 0)
    # instructions and table reads per coordinate of the two rank loops
    for tag, machine in (("S", "server"), ("D", "desktop")):
        for root in R:
            lp = os.path.join(root, "results", machine, "loopstats.json")
            if os.path.exists(lp) and os.path.getsize(lp):
                L_ = json.load(open(lp))
                M[f"hLoopOne{tag}"], M[f"hLoopTwo{tag}"] = str(L_["one_read_instr"]), str(L_["two_read_instr"])
                d0 = 13
                M[f"hLookupOne{tag}"], M[f"hLookupTwo{tag}"] = str(d0 * L_["one_read_instr"]), str(d0 * L_["two_read_instr"])
                M[f"hLoopTwoTen{tag}"] = str(10 * L_["two_read_instr"])          # the two-read loop alone at d = 10

    # ---------------- the default page policy ------------------------------
    for tag, machine, d_ in (("S", "server", qs), ("D", "desktop", qd)):
        pg = group(rowsR(machine, "pages"), lambda r: r["mode"], "query_ns")
        for m, nm in (("absl", "Absl"), ("boost", "Boost"), ("hash", "Umap")):
            if m in pg and m in d_:
                M[f"hPage{nm}{tag}"] = f(100 * (pg[m]["med"] / d_[m]["med"] - 1), 0)
                M[f"hPageRaw{nm}{tag}"] = f(pg[m]["med"], 1)

    # ---------------- overlap against d, batch size ------------------------
    wins = {}
    for tag, machine in (("S", "server"), ("D", "desktop")):
        ws = rowsR(machine, "window")
        wq = group(ws, lambda r: (r["d"], r["mode"]), "query_ns"); wins[tag] = wq
        for W in (8, 16, 32, 64):
            k = (13, f"tapered-ram+batch:{W}")
            M[f"hWin{ {8: 'Eight', 16: 'Sixteen', 32: 'Thirtytwo', 64: 'Sixtyfour'}[W] }{tag}"] = f(wq[k]["med"], 1) if k in wq else PEND
    ds = [6, 8, 10, 13, 15, 16, 20]
    sp6 = Spread(); lines = []
    for tag, name in (("S", "server"), ("D", "desktop")):
        wq = wins[tag]
        def wc(d_, m):
            v = wq.get((d_, m)); return f(sp6.see(v)) if v else "---"
        for label, m in ((f"{name}: array", "tapered-ram"), ("two-read loop", "tapered-ram+two"), ("batched", "tapered-ram+batch"),
                         (r"\texttt{absl}", "absl"), ("open addressing", "oahash")):
            cells = [wc(d_, m) if wq else PEND for d_ in ds]
            lines.append(f"{label} & " + " & ".join(cells) + " \\\\")
        def ovl(d_, m):                        # reads overlapped: dependent time over independent time
            a_, b_ = wq.get((d_, m)), wq.get((d_, m + "+dep"))
            return b_["med"] / a_["med"] if a_ and b_ else None
        for label, m in (("overlap, array", "tapered-ram"), ("overlap, two-read", "tapered-ram+two")):
            lines.append(f"{label} & " + " & ".join(f(ovl(d_, m), 1) if ovl(d_, m) else "---" for d_ in ds) + " \\\\")
        one = [ovl(d_, "tapered-ram") for d_ in ds if ovl(d_, "tapered-ram")]
        if one: M[f"hWinOneMin{tag}"] = f(min(one), 1)
        cut = {"S": 13, "D": 10}[tag]          # first d at which the two-read loop's overlap has fallen
        early = [ovl(d_, "tapered-ram+two") for d_ in ds if d_ < cut and ovl(d_, "tapered-ram+two")]
        late = [ovl(d_, "tapered-ram+two") for d_ in ds if d_ >= cut and ovl(d_, "tapered-ram+two")]
        if early and late:
            M[f"hWinTwoEarlyLo{tag}"], M[f"hWinTwoEarlyHi{tag}"] = f(min(early), 1), f(max(early), 1)
            M[f"hWinTwoLateLo{tag}"], M[f"hWinTwoLateHi{tag}"] = f(min(late), 1), f(max(late), 1)
            assert min(early) > max(late), f"two-read overlap does not fall at d={cut} on {name}"
        if tag == "S":
            lines.append(r"\addlinespace")
    open(os.path.join(a.paper, "tables", "window.tex"), "w").write("\n".join(lines) + "\n")
    M["hSpreadWin"] = sp6.pct()

    # ---------------- sweep -----------------------------------------------
    ss = rowsR("server", "sweep")
    sq = group(ss, lambda r: (r["N"], r["mode"]), "query_ns")
    sm = group(ss, lambda r: (r["N"], r["mode"]), "peak_rss_mib")
    sqb = {r["N"]: r["qbuf_mib"] for r in ss}
    Ns = sorted({k[0] for k in sq})
    sp2 = Spread(); lines = []
    def table_lines(q_d, Nl, spr, mem=True):
        out = []
        for N in Nl:
            def q_(m):
                return f(spr.see(q_d.get((N, m))))
            def m_(m):
                v = sm.get((N, m)); return (v["med"] - sqb[N]) if v else None
            def gib(x):
                return PEND if x is None else f"{x / 1024:.2f}"
            nlabel = f"{N/1e6:.1f}M" if N < 1e9 else f"{N/1e9:.2f}G"
            row = [q_("tapered-ram"), q_("tapered-ram+dep"), q_("tapered-ram+batch"), q_("absl"), q_("absl+dep"), q_("absl+batch"),
                   q_("oahash"), q_("oahash+dep"), q_("oahash+batch"), q_("hash")]
            if mem:
                row += [gib(m_("tapered-ram")), gib(m_("absl")), gib(m_("hash"))]
            out.append(f"{nlabel} & " + " & ".join(row) + " \\\\")
        return out
    open(os.path.join(a.paper, "tables", "scaling.tex"), "w").write("\n".join(table_lines(sq, Ns, sp2) or [r"\pend{--} \\"]) + "\n")
    M["hSpreadSweep"] = sp2.pct()
    if Ns:                                     # structure sizes in the band where only the array fits in the L3 slice
        st0 = {r["mode"]: r["struct_mib"] for r in ss if r["N"] == Ns[0]}
        M["hBandArr"] = f(st0["tapered-ram"], 1)
        hb = [st0[m] for m in ("absl", "oahash", "boost") if m in st0]
        M["hBandLo"], M["hBandHi"] = f(min(hb), 0), f(max(hb), 0)
    # memory headline: server sweep, N >= 20M (fixed overheads dominate below)
    big = [N for N in Ns if N >= 2e7]
    mf = [(sm[(N, m)]["med"] - sqb[N]) / (sm[(N, "tapered-ram")]["med"] - sqb[N])
          for N in big for m in ("oahash", "absl", "boost") if (N, m) in sm]
    mu = [(sm[(N, "hash")]["med"] - sqb[N]) / (sm[(N, "tapered-ram")]["med"] - sqb[N]) for N in big if (N, "hash") in sm]
    M["hMemFlatLo"], M["hMemFlatHi"] = (f(min(mf), 1), f(max(mf), 1)) if mf else (PEND, PEND)
    M["hMemUmapLo"], M["hMemUmapHi"] = (f(min(mu), 1), f(max(mu), 1)) if mu else (PEND, PEND)
    # lookup ratios over N: best flat table over array (above 1 the array is faster)
    def sweep_ratios(q_d, Nl):
        out = {"ind": [], "dep": [], "batch": []}
        for N in Nl:
            for pat, sfx, flats in (("ind", "", FLAT), ("dep", "+dep", FLAT), ("batch", "+batch", ("absl", "oahash"))):
                c = [q_d[(N, m + sfx)]["med"] for m in flats if (N, m + sfx) in q_d]
                a_ = q_d.get((N, "tapered-ram" + sfx))
                if c and a_:
                    out[pat].append((N, min(c) / a_["med"]))
        return out
    def put_range(tag, rr, cache_n, q_d=None):
        for pat, nm in (("ind", "Ind"), ("dep", "Dep"), ("batch", "Batch")):
            v = rr[pat]
            if v:
                sfx = "" if pat == "ind" else "+" + pat
                spr = Spread()
                for N, _ in v:
                    if N >= cache_n:
                        for m in ("tapered-ram", "absl", "oahash", "boost"):
                            spr.see(q_d.get((N, m + sfx)))
                M[f"hIqr{nm}{tag}"] = spr.pct()
                M[f"h{tag}{nm}Lo"], M[f"h{tag}{nm}Hi"] = f(min(x for _, x in v), 2), f(max(x for _, x in v), 2)
                M[f"h{tag}{nm}Top"] = f(v[-1][1], 2)              # at the largest N
                bigv = [x for N, x in v if N >= cache_n]
                if bigv:
                    M[f"h{tag}{nm}BigLo"], M[f"h{tag}{nm}BigHi"] = f(min(bigv), 2), f(max(bigv), 2)
    if Ns:
        put_range("Sweep", sweep_ratios(sq, Ns), 2e7, sq)
        # sizes where a flat table wins independent lookups: how many reads each overlaps there
        lose = [N for N, x in sweep_ratios(sq, Ns)["ind"] if x < 1]
        if lose:
            win = {min(("absl", "oahash", "boost"), key=lambda m: sq[(N, m)]["med"] if (N, m) in sq else 1e9) for N in lose}
            assert win == {"oahash"}, win
            ov = lambda N, m: sq[(N, m + "+dep")]["med"] / sq[(N, m)]["med"]
            oa_, ar_ = [ov(N, "oahash") for N in lose], [ov(N, "tapered-ram") for N in lose]
            M["hSweepOvlOaLo"], M["hSweepOvlOaHi"] = f(min(oa_), 1), f(max(oa_), 1)
            lr = [x for N, x in sweep_ratios(sq, Ns)["ind"] if x < 1]
            M["hSweepIndLoseLo"], M["hSweepIndLoseHi"] = f(min(lr), 2), f(max(lr), 2)
            spl = Spread()
            for N in lose:
                for m in ("tapered-ram", "oahash"): spl.see(sq.get((N, m)))
            M["hIqrIndLose"] = spl.pct()
            M["hSweepOvlArrLo"], M["hSweepOvlArrHi"] = f(min(ar_), 1), f(max(ar_), 1)
            cap = {r["N"]: r["capacity"] for r in ss if r["mode"] == "oahash" and r.get("capacity")}
            lf = [N / cap[N] for N in lose]
            M["hSweepOaLoadLo"], M["hSweepOaLoadHi"] = f(min(lf), 2), f(max(lf), 2)
            M["hSweepLoseCount"] = {1: "largest size", 2: "two largest sizes", 3: "three largest sizes"}.get(len(lose), f"{len(lose)} largest sizes")
            assert lose == Ns[-len(lose):], "the flat table wins at sizes that are not the largest"
        spm = Spread()
        for N in big:
            for m in ("tapered-ram", "absl", "oahash", "boost", "hash"):
                spm.see(sm.get((N, m)))
        M["hIqrMem"] = spm.pct()
        ru = [sq[(N, "hash")]["med"] / sq[(N, "tapered-ram")]["med"] for N in Ns if (N, "hash") in sq and (N, "tapered-ram") in sq]
        if ru: M["hSweepUmapLo"], M["hSweepUmapHi"] = f(min(ru), 1), f(max(ru), 1)
    # desktop sweep: its own sizes, no memory columns
    sd = rowsR("desktop", "sweep")
    if sd:
        spd = Spread()
        dq = group(sd, lambda r: (r["N"], r["mode"]), "query_ns")
        NsD = sorted({k[0] for k in dq})
        open(os.path.join(a.paper, "tables", "scaling_desktop.tex"), "w").write("\n".join(table_lines(dq, NsD, spd, mem=False)) + "\n")
        put_range("Desk", sweep_ratios(dq, NsD), 1e7, dq)
        M["hDeskNLo"] = f(min(NsD) / 1e6, 1); M["hDeskNHi"] = f(max(NsD) / 1e6, 1)
        M["hSpreadSweepDesk"] = spd.pct()
        M["hDeskSweepReps"] = str(max(v["n"] for v in dq.values()))

    # ---------------- parallel --------------------------------------------
    ps = rowsR("server", "parallel")
    def pk(r):
        arr = r["mode"].startswith("array")
        return (r["payload"], r["B"], r["mode"], r["storage"] if arr else "-", r["prefault"] if arr else 0, r["T"])
    pt = group(ps, pk, "total_s")
    pf_ = group(ps, pk, "fill_s")
    bad_ = sum(r.get("errors", 0) for r in ps)
    if bad_:
        sys.exit(f"parallel builds report {bad_} verification errors")
    same = {}                                  # every thread count must write the same bytes
    for r in ps:
        if r["mode"].startswith("array"):
            k = (r["B"], r["payload"], r["mode"] == "array-index")
            if same.setdefault(k, r["content_hash"]) != r["content_hash"]:
                sys.exit(f"array content differs across builds: {k}, T={r['T']}")
    if ps: M["hParMaxT"] = str(max(r["T"] for r in ps if r["mode"].startswith("array")))
    sp3 = Spread()
    def tot(payload, B, mode, storage="-", pf=0, T=1):
        return sp3.see(pt.get((payload, B, mode, storage, pf, T)))
    lines = []
    for T in [1, 4, 16, 64, 128]:
        row = [tot("cheap", 32, "array-index", "shm", 0, T), tot("cheap", 32, "array-index", "anon-thp", 0, T),
               tot("cheap", 32, "array-unrank", "anon-thp", 0, T), tot("cheap", 32, "array-odo", "anon-thp", 0, T),
               tot("cheap", 32, "oa-cas", T=T), tot("cheap", 32, "absl-shard", T=T), tot("cheap", 32, "boost-conc", T=T),
               tot("cheap", 32, "umap-shard", T=T)]
        lines.append(f"{T} & " + " & ".join(f(x, 3 if x is not None and x < 1 else (2 if x is not None and x < 10 else 1)) for x in row) + " \\\\")
    open(os.path.join(a.paper, "tables", "parallel.tex"), "w").write("\n".join(lines) + "\n")
    M["hSpreadPar"] = sp3.pct()
    if pt:
        T = 64                                 # the expensive payload was measured up to 64 threads
        def bestT(m, payload="cheap", B=32):   # each structure at its own best thread count
            st_ = "anon-thp" if m.startswith("array") else "-"
            k = min((k for k in pt if k[:5] == (payload, B, m, st_, 0)), key=lambda k: pt[k]["med"])
            return pt[k]["med"], k[5]
        arr64, M["hParArrT"] = bestT("array-odo"); M["hParArrT"] = str(M["hParArrT"])
        hashes = {m: bestT(m)[0] for m in ("oa-cas", "absl-shard", "boost-conc", "umap-shard")}
        M["hParCasT"] = str(bestT("oa-cas")[1])
        M["hParT"] = str(T)
        M["hParRatio"] = f(hashes["oa-cas"] / arr64, 0)
        M["hParNaive"] = f(hashes["umap-shard"] / arr64, 0)
        a1 = pt[("cheap", 32, "array-odo", "anon-thp", 0, 1)]["med"]
        M["hParArray"] = f(a1 / arr64, 0)
        M["hParHash"] = f(pt[("cheap", 32, "oa-cas", "-", 0, 1)]["med"] / hashes["oa-cas"], 0)
        M["hParNaiveScale"] = f(pt[("cheap", 32, "umap-shard", "-", 0, 1)]["med"] / hashes["umap-shard"], 2)
        M["hParRatioOne"] = f(pt[("cheap", 32, "oa-cas", "-", 0, 1)]["med"] / a1, 0)
        Ts_ = sorted({k[5] for k in pf_ if k[2] == "array-index"})
        N = 1121099408
        def fast(st_, pf):                     # best fill time over thread counts, and its rate
            x = min(pf_[("cheap", 32, "array-index", st_, pf, t)]["med"] for t in Ts_)
            return x, N * 4 / x / 1e9
        for nm, st_, pf in (("Shm", "shm", 0), ("ShmPf", "shm", 1), ("Four", "anon-4k", 0), ("FourPf", "anon-4k", 1),
                            ("Anon", "anon-thp", 0), ("AnonPf", "anon-thp", 1)):
            x, gb = fast(st_, pf)
            M[f"h{nm}Best"] = f(x, 2 if x >= 0.1 else 3); M[f"h{nm}GBs"] = f(gb, 1 if gb < 10 else 0)
        pl64 = {m: pt[("pl", 16, m, "-" if m != "array-odo" else "anon-thp", 0, 64)]["med"]
                for m in ("array-odo", "oa-cas", "absl-shard")}
        M["hParPlRatio"] = f(pl64["oa-cas"] / pl64["array-odo"], 2)
        sc = Spread(); sc.see(pt.get(("cheap", 32, "array-odo", "anon-thp", 0, int(M["hParArrT"])))); sc.see(pt.get(("cheap", 32, "oa-cas", "-", 0, int(M["hParCasT"]))))
        M["hIqrPar"] = sc.pct()
        sc = Spread(); sc.see(pt.get(("pl", 16, "array-odo", "anon-thp", 0, 64))); sc.see(pt.get(("pl", 16, "oa-cas", "-", 0, 64)))
        M["hIqrParPl"] = sc.pct()
        M["hParPlArr"] = f(pl64["array-odo"], 3); M["hParPlCas"] = f(pl64["oa-cas"], 3); M["hParPlAbsl"] = f(pl64["absl-shard"], 3)
        M["hParPlArrOne"] = f(pt[("pl", 16, "array-odo", "anon-thp", 0, 1)]["med"], 1)
        M["hParPlCasOne"] = f(pt[("pl", 16, "oa-cas", "-", 0, 1)]["med"], 1)

    # ---------------- vector-root microbenchmark --------------------------
    pls = rowsR("server", "pl")
    acc = {(r["B"], r["d"], r["path"]): r for r in (rowsR("desktop", "pl") + pls) if r.get("what") == "accuracy"}   # same on both
    lat = [r for r in pls if r.get("what") == "latency" and str(r.get("cell", "")).startswith("pl/lat/")]
    sp4 = Spread(); lines = []
    sp_ = {}
    for (B, d) in [(12, 8), (16, 12), (20, 12), (16, 16)]:
        L = {k: sp4.see(summ([r[k] for r in lat if r["B"] == B and r["d"] == d])) for k in
             ("nearest_ns", "simplex_ns", "newton1_ns", "newton_ns")} if lat else {}
        N = next((r["N"] for r in pls if r.get("B") == B and r.get("d") == d and "N" in r), None)
        def e(path):
            r = acc.get((B, d, path)); return None if r is None else r["err_mean"]
        def s(k):
            return L["newton_ns"] / L[k] if L else None
        sp_[(B, d)] = (s("nearest_ns"), s("simplex_ns"), s("newton1_ns"))
        lines.append(f"$B{{=}}{B},d{{=}}{d}$ & {f(N * 4 / 2**20, 2 if N * 4 / 2**20 < 1 else 1) if N else PEND} & "
                     f"{sci(e('trunc'))} & {sci(e('nearest'))} & {sci(e('simplex'))} & {sci(e('nearest+newton1'))} & "
                     f"{f(L.get('nearest_ns'), 0)} & {f(s('nearest_ns'), 0)} & {f(s('simplex_ns'), 0)} & {f(s('newton1_ns'), 1)} \\\\")
    open(os.path.join(a.paper, "tables", "pl.tex"), "w").write("\n".join(lines) + "\n")
    M["hSpreadPl"] = sp4.pct()
    if lat:
        nearest = [v[0] for v in sp_.values()]
        M["hPlMicroLo"], M["hPlMicroHi"] = f(min(nearest), 0), f(max(nearest), 0)
        M["hNewtonNs"] = f(st.median([r["newton_ns"] for r in lat]), 0)
    latd = [r for r in rowsR("desktop", "pl") if r.get("what") == "latency" and str(r.get("cell", "")).startswith("pl/lat/")]
    if latd: M["hNewtonNsD"] = f(st.median([r["newton_ns"] for r in latd]), 0)
    vh = [r for r in pls if r.get("what") == "verify_hash"]
    if vh:
        M["hVerifyKeys"] = f"{sum(r['keys_checked'] for r in vh):,}".replace(",", "{,}")
        M["hVerifyBad"] = str(sum(r["key_mismatches"] + r["query_mismatches"] for r in vh))

    # ---------------- end-to-end fit --------------------------------------
    fs = [r for r in rowsR("server", "plfit") if r.get("what") == "fit"]
    fsd = [r for r in rowsR("desktop", "plfit") if r.get("what") == "fit"]
    if fs and fsd:                             # iterations and estimates must not depend on the machine
        key = lambda r: (r["B"], r["d"], r["variant"], r.get("inner_rep", r["rep"]))   # one invocation runs R fits
        idx = {key(r): r for r in fsd}
        assert len(idx) == len(fsd), "duplicate fit keys"
        for r in fs:
            o = idx.get(key(r))
            if o and (o["iters"] != r["iters"] or abs(o["lns"] - r["lns"]) > 1e-6 or abs(o["lnc"] - r["lnc"]) > 1e-6):
                sys.exit(f"fit differs across machines: {key(r)}")
    spf = Spread()
    for rs_ in (fs, fsd):
        for k_, v_ in group(rs_, lambda r: (r["B"], r["d"], r["variant"]), "fit_s").items():
            if k_[2] in ("exact", "snewton1"): spf.see(v_)
    M["hSpreadFit"] = spf.pct()
    lines = []
    for (B, d) in [(16, 12), (16, 16)]:
        def sel(rs, v):
            return [r for r in rs if r["B"] == B and r["d"] == d and r["variant"] == v]
        ex_s, ex_d = sel(fs, "exact"), sel(fsd, "exact")
        if not (ex_s or ex_d):
            continue
        for v, label in [("exact", "exact Newton"), ("nearest", "nearest"), ("simplex", "simplex"),
                         ("newton1", r"nearest $+$ Newton"), ("snewton1", r"simplex $+$ Newton"),
                         ("twostage", "nearest, then exact"), ("hash", r"\texttt{absl} memo")]:
            xs, xd = sel(fs, v), sel(fsd, v)
            x = xs or xd                       # estimates and iterations agree across machines
            if not x:
                lines.append(f"$d{{=}}{d}$, {label} & " + " & ".join([PEND] * 8) + " \\\\"); continue
            def tcols(xx, ee):
                if not ee: return [PEND, PEND, PEND]
                if not xx: return ["---", "---", "---"]
                t = st.median([r["fit_s"] for r in xx]); te = st.median([r["fit_s"] for r in ee])
                return [f"{st.median([r['ns_per_eval'] for r in xx]):.0f}", f"{t:.2f}", f"{te / t:.2f}"]
            if v == "exact":
                dz = "0 [0]"; dc = "0 [0]"
            else:
                zs = [abs(r["dz_lns"]) for r in x]; zc = [abs(r["dz_lnc"]) for r in x]
                dz = f"{st.median(zs):.2f} [{max(zs):.2f}]"; dc = f"{st.median(zc):.2f} [{max(zc):.2f}]"
            lines.append(f"$d{{=}}{d}$, {label} & {st.median([r['iters'] for r in x]):.0f} & "
                         + " & ".join(tcols(xs, ex_s) + tcols(xd, ex_d)) + f" & {dz} & {dc} \\\\")
        lines.append(r"\addlinespace")
    open(os.path.join(a.paper, "tables", "fit.tex"), "w").write("\n".join(lines) + "\n")
    def fitv(B, d, v):
        return [r for r in fs if r["B"] == B and r["d"] == d and r["variant"] == v]
    ex = fitv(16, 16, "exact")
    if ex:
        t_ex = st.median([r["fit_s"] for r in ex])
        best = None
        for v in ("snewton1", "newton1"):
            x = fitv(16, 16, v)
            if x:
                best = (v, t_ex / st.median([r["fit_s"] for r in x]),
                        max(max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for r in x)); break
        if best:
            M["hFitVariant"] = best[0]
            M["hFitSpeed"] = f(best[1], 1); M["hFitDz"] = f(best[2], 2)
        raw = [max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for r in fitv(16, 12, "nearest") + fitv(16, 16, "nearest")]
        if raw:
            M["hFitDzRawMed"] = f(st.median(raw), 1); M["hFitDzRawMax"] = f(max(raw), 1)
        M["hFitExactS"] = f(t_ex, 2)
        M["hFitSE"] = f(st.median([r["se_lns"] for r in ex]), 4)

    def fitd(B, d, v):
        return [r for r in fsd if r["B"] == B and r["d"] == d and r["variant"] == v]
    sp_d = []
    for (B, d) in [(16, 12), (16, 16)]:
        e_, x_ = fitd(B, d, "exact"), fitd(B, d, "snewton1")
        if e_ and x_:
            sp_d.append(st.median([r["fit_s"] for r in e_]) / st.median([r["fit_s"] for r in x_]))
            M["hFitSNOneDzD"] = f(max(max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for (BB, dd) in [(16, 12), (16, 16)] for r in fitd(BB, dd, "snewton1")), 2)
            M["hFitSNOneItD"] = f(st.median([r["iters"] for r in x_]), 0)
            M["hFitSNOneEvalD"] = f(st.median([r["ns_per_eval"] for r in e_]) / st.median([r["ns_per_eval"] for r in x_]), 1)
            M["hFitSNOnedLLD"] = f(-min(r["dLL_exact"] for (BB, dd) in [(16, 12), (16, 16)] for r in fitd(BB, dd, "snewton1")), 3)
    if sp_d: M["hFitSNOneSpeedDLo"], M["hFitSNOneSpeedDHi"] = f(min(sp_d), 1), f(max(sp_d), 1)
    sp_all = list(sp_d)
    for (B, d) in [(16, 12), (16, 16)]:
        e_, x_ = fitv(B, d, "exact"), fitv(B, d, "snewton1")
        if e_ and x_: sp_all.append(st.median([r["fit_s"] for r in e_]) / st.median([r["fit_s"] for r in x_]))
    if sp_all: M["hFitSNOneSpeedLo"], M["hFitSNOneSpeedHi"] = f(min(sp_all), 1), f(max(sp_all), 1)
    # break-even: table build (all threads) over the time one fit saves, on the machine it ran on
    for tag, machine, sel in (("S", "server", fitv), ("D", "desktop", fitd)):
        bl = [r for r in rowsR(machine, "pl") if r.get("what") == "build" and not r.get("reused")]
        for nm, (B, d) in (("Tw", (16, 12)), ("Sx", (16, 16))):
            b_ = [r["build_s"] for r in bl if (r["B"], r["d"]) == (B, d)]
            e_, x_ = sel(B, d, "exact"), sel(B, d, "snewton1")
            if b_ and e_ and x_:
                save = st.median([r["fit_s"] for r in e_]) - st.median([r["fit_s"] for r in x_])
                M[f"hBuild{nm}{tag}"] = f(b_[0], 0 if b_[0] >= 10 else 1); M[f"hBreak{nm}{tag}"] = f(b_[0] / save, 0 if b_[0] / save >= 10 else 1)
    xs_ = [r for (B, d) in [(16, 12), (16, 16)] for r in fitv(B, d, "snewton1") + fitd(B, d, "snewton1")]
    if xs_: M["hFitSNOneDzAll"] = f(max(max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for r in xs_), 2)
    its = [st.median([r["iters"] for r in fitd(BB, dd, "snewton1")]) for (BB, dd) in [(16, 12), (16, 16)] if fitd(BB, dd, "snewton1")]
    if its: M["hFitSNOneItD"] = f(min(its), 0) if min(its) == max(its) else f"{min(its):.0f}--{max(its):.0f}"
    e_, x_ = fitd(16, 16, "exact"), fitd(16, 16, "newton1")
    if e_ and x_: M["hFitNOneSpeedD"] = f(st.median([r["fit_s"] for r in e_]) / st.median([r["fit_s"] for r in x_]), 1)
    rawx = [max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for v in ("nearest", "simplex") for (BB, dd) in [(16, 12), (16, 16)] for r in (fitv(BB, dd, v) or fitd(BB, dd, v))]
    if rawx: M["hFitDzRawMax"] = f(max(rawx), 1)

    # ---------------- entmax ----------------------------------------------
    es = [r for r in rowsR("server", "entmax") if r.get("what") == "latency"]
    if es:
        M["hEntMemo"] = f(st.median([r["memo_ns"] for r in es]), 0)
        M["hEntExact"] = f(st.median([r["exact_ns"] for r in es]), 0)
    ea = [r for r in rowsR("server", "entmax") if r.get("what") == "accuracy"]
    if ea:
        M["hEntErr"] = sci(ea[0]["err_mean"]); M["hEntSuppMed"] = f(ea[0]["support_median"], 0); M["hEntSuppMean"] = f(ea[0]["support_mean"], 1)

    # ---------------- out of core -----------------------------------------
    oo = rowsR("server", "ooc")
    cold = [r for r in oo if r.get("cell") == "ooc/query-cold"]; warm = [r for r in oo if r.get("cell") == "ooc/query-warm"]
    if cold:
        M["hNvmeColdUs"] = f(st.median([r["p50_us"] for r in cold]), 0)
        M["hNvmeWarmUs"] = f(st.median([r["p50_us"] for r in warm]), 2)
    od = rowsR("desktop", "ooc")
    def walk(prefix, addr):
        return [r for r in od if str(r.get("cell", "")).startswith(prefix) and r.get("addressing") == addr and r.get("phase") == "measured"]
    def changed(r):                             # hit rate over steps that moved to another gridpoint
        st_ = [float(x) for x in r["steps_by_coord"].split(",")]; hp = [float(x) for x in r["hit_pct_by_coord"].split(",")]
        n = sum(st_[:-1]); return sum(a_ * max(b_, 0) for a_, b_ in zip(st_[:-1], hp[:-1])) / n if n else None
    def same(r):
        st_ = [float(x) for x in r["steps_by_coord"].split(",")]; return 100 * st_[-1] / sum(st_)
    sp5 = Spread(); lines = []
    def wmed(rs, fn):
        v = [fn(r) for r in rs]; v = [x for x in v if x is not None]
        return sp5.see(summ(v)) if v else None
    for label, pre_ in [(r"uniform $\pm1$ walk, cold", "walk/uniform-cold/"), (r"same, warm", "walk/uniform-warm/"),
                        (r"Gaussian, $\sigma=0.25$, cold", "walk/gauss0.25/"), (r"same, warm", "walk/gausswarm0.25/"),
                        (r"Gaussian, $\sigma=0.5$, cold", "walk/gauss0.5/"), (r"replayed fit trace, cold", "walk/pltrace/")]:
        l, h = walk(pre_, "lex"), walk(pre_, "hashed")
        hit_l, hit_h = wmed(l, lambda r: r["hit_pct"]), wmed(h, lambda r: r["hit_pct"])
        ch_l, ch_h = wmed(l, changed), wmed(h, changed)
        ms_l, ms_h = wmed(l, lambda r: r["mean_us"] / 1000), wmed(h, lambda r: r["mean_us"] / 1000)
        sm_ = st.median([same(r) for r in l]) if l else None
        lines.append(f"{label} & {f(sm_, 0)} & {f(hit_l)} & {f(hit_h)} & {f(ch_l)} & {f(ch_h)} & {f(ms_l, 2)} & {f(ms_h, 2)} \\\\")
        if not (l and h):
            continue
        tt = st.median([r["total_s"] for r in h]) / st.median([r["total_s"] for r in l])
        if pre_ == "walk/uniform-cold/":
            M["hWalkLex"], M["hWalkHash"] = f(hit_l, 0), f(hit_h, 0)
            M["hWalkTime"] = f(tt, 1)
            M["hWalkMissLex"], M["hWalkMissHash"] = f(st.median([r["miss_mean_ms"] for r in l]), 1), f(st.median([r["miss_mean_ms"] for r in h]), 1)
            ns = None
            for r in l:                         # hit rate per coordinate, seeds pooled
                st_ = [float(x) for x in r["steps_by_coord"].split(",")][:-1]; hp = [float(x) for x in r["hit_pct_by_coord"].split(",")][:-1]
                if ns is None: ns, hs_ = [0.0] * len(st_), [0.0] * len(st_)
                for i_, (n_, p_) in enumerate(zip(st_, hp)):
                    ns[i_] += n_; hs_[i_] += n_ * max(p_, 0) / 100
            hp = [100 * x / n_ if n_ else 0 for x, n_ in zip(hs_, ns)]
            M["hWalkLeadHi"] = f(max(hp[:8]), 0); M["hWalkTrailLo"] = f(min(hp[-4:]), 0)
        if pre_ == "walk/uniform-warm/":
            M["hWarmLex"], M["hWarmHash"] = f(hit_l, 0), f(hit_h, 0); M["hWarmTime"] = f(tt, 1)
            M["hWarmResLex"] = f(st.median([r["resident_before_pct"] for r in l]), 0)
            M["hWarmResHash"] = f(st.median([r["resident_before_pct"] for r in h]), 0)
        if pre_ == "walk/gausswarm0.25/":
            M["hGaussWarmLexCh"] = f(ch_l, 0); M["hGaussWarmHashCh"] = f(ch_h, 0)
        if pre_ == "walk/gauss0.25/":
            M["hGaussSame"] = f(sm_, 0); M["hGaussLexCh"] = f(ch_l, 0); M["hGaussHashCh"] = f(ch_h, 0)
        if pre_ == "walk/gauss0.5/":
            M["hGaussWideLex"], M["hGaussWideHash"] = f(hit_l, 1), f(hit_h, 1)
        if pre_ == "walk/pltrace/":
            M["hTraceLex"], M["hTraceHash"] = f(hit_l, 0), f(hit_h, 0)
            M["hTraceSame"] = f(sm_, 0); M["hTraceLexCh"] = f(ch_l, 0); M["hTraceHashCh"] = f(ch_h, 0)
            M["hTraceTime"] = f(tt, 1)
            M["hTraceM"] = f"{l[0]['M']:,}".replace(",", "{,}")
            sc = Spread(); sc.see(summ([r["hit_pct"] for r in l])); sc.see(summ([r["hit_pct"] for r in h])); M["hIqrTrace"] = sc.pct()
    M["hSpreadWalk"] = sp5.pct()
    # desktop out-of-core build / scan / cold point queries
    ob = [r for r in od if r.get("cell") == "ooc/build"]; osc = [r for r in od if r.get("cell") == "ooc/scan"]
    oq = [r for r in od if r.get("cell") == "ooc/query-cold"]
    oqw = [r for r in od if r.get("cell") == "ooc/query-warmed"]
    if oqw:
        M["hOocWarmHit"] = f(st.median([r["hit_pct"] for r in oqw]), 0)
        M["hOocWarmMs"] = f(st.median([r["mean_us"] for r in oqw]) / 1000, 1)
    badq = [r for r in od if r.get("checksum_ok") == "NO" or r.get("payload_hash_ok") == "NO"]
    if badq:
        sys.exit(f"out-of-core answers or checksum wrong in {badq[0].get('cell')}")
    if ob:
        M["hOocBuildS"] = f(ob[0]["build_s"], 0); M["hOocBuildMBs"] = f(ob[0]["file_gb"] * 1e3 / ob[0]["build_s"], 0)
        M["hOocGB"] = f(ob[0]["file_gb"], 1)
    if osc:
        M["hOocScanS"] = f(osc[0]["scan_s"], 0); M["hOocScanMBs"] = f(ob[0]["file_gb"] * 1e3 / osc[0]["scan_s"], 0) if ob else PEND
        M["hOocScanBad"] = str(osc[0]["mismatches"])
    if oq:
        M["hOocColdMs"] = f(st.median([r["p50_us"] for r in oq]) / 1000, 1)
        M["hOocColdMean"] = f(st.median([r["mean_us"] for r in oq]) / 1000, 1)      # compare with hOocWarmMs (a mean)
        M["hOocColdMiss"] = f(100 - st.median([r["hit_pct"] for r in oq]), 1)
        M["hOocColdN"] = str(len(oq))
    wv = [r["mem_avail_gib"] for r in od if "mem_avail_gib" in r and str(r.get("cell", "")).startswith("walk/")]
    if wv and ob:
        avail = st.median(wv)
        M["hDeskAvail"] = f(avail, 1); M["hOocCacheRatio"] = f(ob[0]["file_gb"] * 1e9 / 2**30 / avail, 1)
    # what key-storing in-RAM tables would need at the desktop N: open addressing by its
    # capacity rule (16-byte slots, power-of-two capacity, load <= 7/8); unordered_map by its
    # measured bytes per entry at the largest server sweep size
    if ob:
        No = ob[0]["N"]; cap = 1
        while No / cap > 0.875: cap <<= 1
        M["hOocKeyLo"] = f(cap * 16 / 1e9, 0)
        if Ns and (max(Ns), "hash") in sm:
            Nb = max(Ns); bpe = (sm[(Nb, "hash")]["med"] - sqb[Nb]) * 2**20 / Nb
            M["hOocKeyHi"] = f(bpe * No / 1e9, 0)
    open(os.path.join(a.paper, "tables", "walk.tex"), "w").write("\n".join(lines) + "\n")

    # ---------------- per-structure lookup macros (prose) -------------------
    names = {"rankonly": "Rank", "rankonly+two": "RankTwo", "tapered-ram": "Arr", "tapered-ram+dep": "ArrDep",
             "tapered-ram+batch": "ArrBatch", "tapered-ram+two": "Two", "tapered-ram+two+dep": "TwoDep",
             "tapered-ram+two+batch": "TwoBatch", "tapered-ram+pre": "Pre", "tapered-ram+pre+dep": "PreDep",
             "tapered": "File", "tapered+batch": "FileBatch", "tapered+pin": "FilePin", "tabled": "Tab", "tabled+dep": "TabDep",
             "absl": "Absl", "absl+dep": "AbslDep", "absl+batch": "AbslBatch", "boost": "Boost", "boost+dep": "BoostDep",
             "oahash": "Oa", "oahash+dep": "OaDep", "oahash+batch": "OaBatch", "hash": "Umap", "hash+dep": "UmapDep"}
    for tag, dq_ in (("S", qs), ("D", qd)):
        for mode, nm in names.items():
            M[f"q{tag}{nm}"] = f(dq_[mode]["med"], 1) if mode in dq_ else PEND
    if "tapered+pin" in qs and "tabled" in qs:
        M["hTabGapS"] = f(qs["tapered+pin"]["med"] - qs["tabled"]["med"], 0)
        M["hDomainS"] = f(qs["tapered"]["med"] - qs["tapered+pin"]["med"], 0)
    if "tapered+pin" in qd and "tabled" in qd:
        M["hTabGapD"] = f(qd["tapered+pin"]["med"] - qd["tabled"]["med"], 0)
        M["hDomainD"] = f(qd["tapered"]["med"] - qd["tapered+pin"]["med"], 0)
    if "tapered" in qs and "tapered-ram" in qs:
        M["hFileCost"] = f(qs["tapered"]["med"] - qs["tapered-ram"]["med"], 0)
    if "tapered-ram" in ms:
        M["hStructArr"] = f(ms["tapered-ram"]["med"] - qbuf, 0)
    for mode, nm in (("absl", "Absl"), ("oahash", "Oa"), ("hash", "Umap"), ("boost", "Boost")):
        if mode in ms and "tapered-ram" in ms:
            M[f"hRss{nm}"] = f((ms[mode]["med"] - qbuf) / (ms["tapered-ram"]["med"] - qbuf), 1)
    # bytes per entry at large N
    if Ns:
        Nb = max(Ns); v = sm.get((Nb, "tapered-ram"))
        if v: M["hBperEntry"] = f((v["med"] - sqb[Nb]) * 2**20 / Nb, 2)

    # ---------------- PL prose macros ---------------------------------------
    def em(B, d, p):
        r = acc.get((B, d, p)); return None if r is None else r["err_mean"]
    if acc:
        import math
        M["hTruncEi"] = sci(em(12, 8, "trunc"))       # truncation depends on d alone
        M["hTruncTw"] = sci(em(16, 12, "trunc")); M["hTruncSx"] = sci(em(16, 16, "trunc"))
        Nd = {d_: math.comb(16 + d_ - 2, d_ - 1) for d_ in (8, 12, 16)}      # B = 16, first coordinate pinned
        M["hNEi"], M["hNTw"], M["hNSx"] = sci(Nd[8]), sci(Nd[12]), sci(Nd[16])
        M["hTaperGrow"] = f(Nd[16] / Nd[12], 0); M["hTaperShrink"] = f(em(16, 12, "trunc") / em(16, 16, "trunc"), 0)
        M["hNearTw"] = sci(em(16, 12, "nearest")); M["hNearSx"] = sci(em(16, 16, "nearest"))
        M["hSimpTw"] = sci(em(16, 12, "simplex")); M["hSimpSx"] = sci(em(16, 16, "simplex"))
        M["hSimpGain"] = f(em(16, 16, "nearest") / em(16, 16, "simplex"), 1)
        n1 = [em(B, d, "nearest+newton1") for (B, d) in [(16, 12), (20, 12), (16, 16)]]
        M["hNOneErrLo"], M["hNOneErrHi"] = sci(min(n1)), sci(max(n1))
        sps = [v[2] for k, v in sp_.items() if k != (12, 8) and v[2]]
        if sps: M["hNOneSpeedLo"], M["hNOneSpeedHi"] = f(min(sps), 1), f(max(sps), 1)
        ssp = [v[1] for k, v in sp_.items() if v[1]]
        if ssp: M["hSimpSpeedLo"], M["hSimpSpeedHi"] = f(min(ssp), 0), f(max(ssp), 0)
        if sp_.get((16, 16)) and sp_[(16, 16)][1]: M["hSimpSpeedSx"] = f(sp_[(16, 16)][1], 0)

    # ---------------- fit prose macros --------------------------------------
    if fs:
        def med_(v, key, B=None, d=None):
            x = [r[key] for r in fs if r["variant"] == v and (B is None or (r["B"], r["d"]) == (B, d))]
            return st.median(x) if x else None
        M["hFitExactNs"] = f(med_("exact", "ns_per_eval"), 0)
        M["hFitExactIt"] = f(med_("exact", "iters"), 0)
        M["hFitNOneIt"] = f(med_("newton1", "iters"), 0) if med_("newton1", "iters") else PEND
        if med_("newton1", "ns_per_eval"):
            M["hFitNOnePerEval"] = f(med_("exact", "ns_per_eval") / med_("newton1", "ns_per_eval"), 1)
            x = [r for r in fs if r["variant"] == "newton1"]
            M["hFitNOneDz"] = f(max(max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for r in x), 2)
            M["hFitNOneSpeed"] = f(med_("exact", "fit_s") / med_("newton1", "fit_s"), 1)
            M["hFitNOnedLL"] = f(-min(r["dLL_exact"] for r in x), 3)
        x = [r for r in fs if r["variant"] == "snewton1"]
        if x:
            M["hFitSNOneDz"] = f(max(max(abs(r["dz_lns"]), abs(r["dz_lnc"])) for r in x), 2)
            M["hFitSNOneSpeed"] = f(med_("exact", "fit_s") / med_("snewton1", "fit_s"), 1)
            M["hFitSNOneIt"] = f(med_("snewton1", "iters"), 0)
        raws = [med_("exact", "fit_s", B, d) / med_(v, "fit_s", B, d) for v in ("nearest", "simplex")
                for (B, d) in [(16, 12), (16, 16)] if med_(v, "fit_s", B, d)]
        for (B, d) in [(16, 12), (16, 16)]:   # the desktop too
            e_ = [r["fit_s"] for r in fsd if (r["B"], r["d"], r["variant"]) == (B, d, "exact")]
            for v in ("nearest", "simplex"):
                x_ = [r["fit_s"] for r in fsd if (r["B"], r["d"], r["variant"]) == (B, d, v)]
                if e_ and x_: raws.append(st.median(e_) / st.median(x_))
        if raws: M["hFitRawSpeedLo"], M["hFitRawSpeedHi"] = f(min(raws), 1), f(max(raws), 1)
        x = [abs(r["dz_lns"]) for r in fs if r["variant"] == "nearest" and r["d"] == 16]      # as in Table fit
        if x: M["hFitSxNearMed"] = f(st.median(x), 1)
        x = [abs(r["dz_lns"]) for r in fs if r["variant"] == "nearest" and r["d"] == 12]
        if x: M["hFitTwNearMed"] = f(st.median(x), 1)

    # ---------------- parallel prose macros ---------------------------------
    if pt:
        for m, nm in (("array-odo", "Arr"), ("oa-cas", "Cas"), ("absl-shard", "AbslSh"), ("boost-conc", "BoostC"), ("umap-shard", "UmapSh")):
            st_ = "anon-thp" if m.startswith("array") else "-"
            M[f"hPar{nm}One"] = f(pt[("cheap", 32, m, st_, 0, 1)]["med"], 1)
            b_, T_ = bestT(m)                  # best over thread counts
            M[f"hPar{nm}Sf"] = f(b_, 3 if b_ < 1 else 1); M[f"hPar{nm}SfT"] = str(T_)
        pr = group(ps, lambda r: (r["mode"], r["B"], r["payload"]), "peak_rss_mib")
        M["hParArrRss"] = f(pr[("array-odo", 32, "cheap")]["med"], 0)
        M["hParCasRss"] = f(pr[("oa-cas", 32, "cheap")]["med"] / 1024, 0)
        M["hParUmapRss"] = f(pr[("umap-shard", 32, "cheap")]["med"] / 1024, 0)

    # ---------------- taper example, penalty check ---------------------------
    for root in R:
        tp = os.path.join(root, "results", "desktop", "taper.json")
        if os.path.exists(tp):
            T_ = json.load(open(tp)); g_ = T_["g"]
            M["hTaperR"] = f(T_["r"], 2)
            rat = [g_[i + 1] / g_[i] for i in range(len(g_) - 1)]    # rat[i]: coordinate i+2 over i+1, 1-based
            M["hTaperRatSix"] = f(rat[4], 2); M["hTaperRatSeventeen"] = f(rat[15], 2)
        pp = os.path.join(root, "results", "desktop", "penalty.log")
        if os.path.exists(pp):
            e1, e2 = [], []
            for ln in open(pp):
                x = ln.split()
                if len(x) == 10 and x[3] == "|" and x[7] == "|" and x[0] != "r":
                    e1.append(abs(float(x[6]))); e2.append(abs(float(x[9])))
            if e1:
                M["hPenCells"] = str(len(e1)); M["hPenLo"], M["hPenHi"] = f(min(e1), 1), f(max(e1), 1)
                M["hPenMed"] = f(st.median(e1), 1); M["hAsyLo"], M["hAsyHi"] = f(min(e2), 0), f(max(e2), 0)

    # ---------------- run counts --------------------------------------------
    M["hReps"] = str(max((v["n"] for v in qs.values()), default=0)) if qs else PEND
    M["hRepsDesk"] = str(max((v["n"] for v in qd.values()), default=0)) if qd else PEND

    M.setdefault("hOocGB", "22.3")             # 4 * C(36,15) bytes: a property of the shape
    import re
    # every lookup configuration on both machines: best flat table over array, by access pattern
    cmp_ = {"ind": [], "dep": [], "batch": []}
    for m_ in ("server", "desktop"):
        for p_ in ("h2h", "window", "sweep"):
            gq = group([r for r in rowsR(m_, p_) if "query_ns" in r], lambda r: (r["N"], r["d"], r["mode"]), "query_ns")
            for (N_, d_) in sorted({k[:2] for k in gq}):
                for pat, sfx in (("ind", ""), ("dep", "+dep"), ("batch", "+batch")):
                    a_ = gq.get((N_, d_, "tapered-ram" + sfx))
                    c_ = [gq[(N_, d_, m + sfx)]["med"] for m in FLAT if (N_, d_, m + sfx) in gq]
                    if a_ and c_:
                        cmp_[pat].append(min(c_) / a_["med"])
    if cmp_["dep"]:
        M["hAllDepMin"], M["hAllBatchMin"] = f(min(cmp_["dep"]), 2), f(min(cmp_["batch"]), 2)
        M["hAllIndWorst"], M["hAllIndHi"] = f(1 / min(cmp_["ind"]), 1), f(max(cmp_["ind"]), 1)
        assert min(cmp_["dep"]) > 1 and min(cmp_["batch"]) > 1, "a flat table won dependent or batched lookups somewhere"
    if "hIndS" in M and "hIndD" in M and PEND not in (M["hIndS"], M["hIndD"]):   # independent lookups at N=37.4M
        M["hIndGap"] = f(100 * max(abs(1 - float(M["hIndS"])), abs(1 - float(M["hIndD"]))), 0)
    if "tabled" in qd and "tapered+pin" in qd:
        M["hTabRatioD"] = f(qd["tabled"]["med"] / qd["tapered+pin"]["med"], 1)

    # the query stream's working set (analysis/query_stream.py)
    for root in R:
        qp = os.path.join(root, "results", "query_stream.json")
        if os.path.exists(qp):
            qsj = json.load(open(qp))
            ha, hh = [g_["hot32_array"] for g_ in qsj], [g_["hot32_hash"] for g_ in qsj]
            M["hHotArrLo"], M["hHotArrHi"] = f(100 * min(ha), 0), f(100 * max(ha), 0)
            M["hHotHashLo"], M["hHotHashHi"] = f(100 * min(hh), 0), f(100 * max(hh), 0)

    # other tenants' load on the shared server, by plan (sampled once a minute)
    for root in R:
        lt, sp_ = (os.path.join(root, "results", "server", n) for n in ("other_load.tsv", "other_load_spans.tsv"))
        if os.path.exists(lt) and os.path.exists(sp_):
            ld = [l.rstrip("\n").split("\t") for l in open(lt)][1:]
            spans = {f_[0]: (f_[1], f_[2]) for f_ in (l.rstrip("\n").split("\t") for l in open(sp_) if l.strip() and not l.startswith("#")) if f_[0] != "plan"}
            def other(plan):
                a_, b_ = spans[plan]
                v = [float(r[5]) for r in ld if a_ <= r[0][11:16] < b_]
                return st.mean(v) if v else None
            M["hOtherSweep"], M["hOtherPar"] = f(other("sweep"), 1), f(other("parallel"), 1)
            M["hOtherHead"] = f(other("h2h"), 2)

    # the listing, from the source that `make verify` compiles and runs
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "listing1.cpp")).read().splitlines()
    i0, i1 = next(i for i, l in enumerate(src) if "BEGIN LISTING" in l), next(i for i, l in enumerate(src) if "END LISTING" in l)
    ind = min(len(l) - len(l.lstrip()) for l in src[i0 + 1:i1] if l.strip())
    open(os.path.join(a.paper, "tables", "listing1.cpp"), "w").write("\n".join(l[ind:] for l in src[i0 + 1:i1]) + "\n")

    used = set(re.findall(r"\\((?:h|q[SD])[A-Z][A-Za-z]*)", open(os.path.join(a.paper, "main.tex")).read()))
    missing = sorted(used - set(M))
    for k in missing:
        M[k] = PEND
    if missing:
        print("NOT YET MEASURED (pending):", " ".join(missing), file=sys.stderr)
    with open(os.path.join(a.paper, "numbers_pending.tex"), "w") as fh:
        fh.write("%% GENERATED: every macro, pending colour; numbers.tex overrides with measured values.\n")
        for k in sorted(M):
            fh.write(f"\\newcommand{{\\{k}}}{{{PEND}}}\n")
    with open(os.path.join(a.paper, "numbers.tex"), "w") as fh:
        fh.write("%% GENERATED by rev1-bench/analysis/make_numbers.py -- do not edit by hand.\n")
        for k in sorted(M):
            fh.write(f"\\def\\{k}{{{M[k]}}}\n")      # overrides numbers_pending.tex
    for k in sorted(M):
        print(f"{k:16s} {M[k]}")


def check_answers(rs):
    """Every structure must return the same answers: same checksum per query stream."""
    seen = {}
    for r in rs:
        if r.get("base_mode") in (None, "rankonly") or r.get("rc", 0) != 0 or "checksum" not in r:
            continue
        k = (r["machine"], r["B"], r["d"], r["M"], r["seed"], r.get("pin", 0))
        v = seen.setdefault(k, r["checksum"])
        if v != r["checksum"]:                 # same floats summed in the same order: exact
            sys.exit(f"CHECKSUM MISMATCH {k}: {v} vs {r['checksum']} ({r['mode']})")


def sci(x):
    if x is None:
        return PEND
    m, e = f"{x:.1e}".split("e")
    return f"${m}\\cdot10^{{{int(e)}}}$"


if __name__ == "__main__":
    main()
