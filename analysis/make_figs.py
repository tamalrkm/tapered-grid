"""Figures for the paper from the raw logs: scaling.pdf and locality.pdf.

usage: python3 analysis/make_figs.py --results <root> [<root> ...] --out <paper dir>/figs
Needs matplotlib. Medians over runs, as in the tables.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from load import rows, group  # noqa: E402
import matplotlib
matplotlib.use("pdf")
import matplotlib.pyplot as plt  # noqa: E402

# categorical slots in fixed order (validated: CVD and normal-vision separation pass);
# every series also has its own marker, so identity never rests on colour alone
SER = [("tapered-ram", "array", "#2a78d6", "o"),
       ("absl", "absl", "#eb6834", "s"),
       ("oahash", "open addr.", "#1baf7a", "^"),
       ("hash", "unordered_map", "#eda100", "D")]
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({"font.size": 8, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2,
                     "ytick.color": INK2, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.5,
                     "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False,
                     "pdf.fonttype": 42, "lines.linewidth": 1.5, "lines.markersize": 4})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    R = lambda m, p: [r for root in a.results for r in rows(root, m, p)]
    ss = R("server", "sweep") or R("desktop", "sweep")          # preview until the server run exists
    q = group(ss, lambda r: (r["N"], r["mode"]), "query_ns")
    ms = group(ss, lambda r: (r["N"], r["mode"]), "peak_rss_mib")
    qb = {r["N"]: r["qbuf_mib"] for r in ss}

    fig, ax = plt.subplots(1, 4, figsize=(7.0, 2.25), constrained_layout=True)
    for axis, sfx, title in zip(ax[:3], ("", "+dep", "+batch"), ("independent", "dependent", "batched")):
        for mode, label, col, mk in SER:
            pts = sorted((N, v["med"]) for (N, mm), v in q.items() if mm == mode + sfx)
            if pts:
                axis.plot([p[0] for p in pts], [p[1] for p in pts], color=col, marker=mk, label=label)
        axis.set_xscale("log"); axis.set_xlabel("entries $N$"); axis.set_title(title, fontsize=8, color=INK)
    top = max(x.get_ylim()[1] for x in ax[:3])
    for x in ax[:3]:
        x.set_ylim(0, top)
    ax[0].set_ylabel("ns per lookup")
    for mode, label, col, mk in SER:
        pts = sorted((N, (v["med"] - qb[N]) / 1024) for (N, mm), v in ms.items() if mm == mode)
        if pts:
            ax[3].plot([p[0] for p in pts], [p[1] for p in pts], color=col, marker=mk, label=label)
    ax[3].set_xscale("log"); ax[3].set_yscale("log"); ax[3].set_xlabel("entries $N$"); ax[3].set_ylabel("GiB")
    ax[3].set_title("memory", fontsize=8, color=INK)
    ax[0].legend(loc="upper left", fontsize=6.5)
    fig.savefig(os.path.join(a.out, "scaling.pdf"), metadata={"CreationDate": None, "ModDate": None})

    # locality: hit rate by the coordinate a step changed, uniform cold walk, both seeds pooled
    od = R("desktop", "ooc")
    fig, axis = plt.subplots(figsize=(4.6, 2.3), constrained_layout=True)
    for addr, label, col, mk in (("lex", "lexicographic", "#2a78d6", "o"), ("hashed", "hashed", "#eb6834", "s")):
        rs = [r for r in od if str(r.get("cell", "")).startswith("walk/uniform-cold/") and r.get("addressing") == addr and r.get("phase") == "measured"]
        if not rs:
            continue
        d = len(rs[0]["steps_by_coord"].split(",")) - 1
        tot = [0.0] * d; hit = [0.0] * d
        for r in rs:
            st_ = [float(x) for x in r["steps_by_coord"].split(",")][:d]
            hp = [float(x) for x in r["hit_pct_by_coord"].split(",")][:d]
            for i in range(d):
                tot[i] += st_[i]; hit[i] += st_[i] * max(hp[i], 0) / 100
        axis.plot(range(1, d + 1), [100 * h / t if t else 0 for h, t in zip(hit, tot)], color=col, marker=mk, label=label)
    axis.set_xlabel("coordinate changed by the step"); axis.set_ylabel("page-cache hits, %")
    axis.set_ylim(0, 102); axis.set_xticks(range(1, 16)); axis.legend(loc="upper left", fontsize=7)
    fig.savefig(os.path.join(a.out, "locality.pdf"), metadata={"CreationDate": None, "ModDate": None})


if __name__ == "__main__":
    main()
