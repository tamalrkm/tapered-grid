#!/usr/bin/env python3
"""Run the revision's measurement plans and collect JSON results.

    python3 scripts/run.py --machine server  --plans all
    python3 scripts/run.py --machine desktop --plans all --ooc-dir /data/tg-ooc
    python3 scripts/run.py --machine server  --plans h2h --dry-run

Results: results/<machine>/<plan>.jsonl, one JSON object per measurement, each
tagged with the command, repetition, load average before/after, NUMA binding
and wall time. results/<machine>/env.json records the machine. Re-running skips
every (plan, cell, rep) already recorded, so an interrupted run resumes.

Repetitions are interleaved: rep r runs every cell of the plan once, in an order
shuffled with seed r, before rep r+1 starts. Slow drift on a shared machine
therefore affects all structures alike instead of whichever ran last.

Standard library only. On the desktop, run it from a text console with VS Code
and the browser closed: the out-of-core plans need the page cache.
"""
import argparse, json, os, platform, random, shlex, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=60).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"<{e}>"


def mem_available_gib():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / 2**20
    return 0.0


def env_record(machine):
    return {
        "machine": machine, "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "uname": platform.uname()._asdict(), "lscpu": sh("lscpu"), "free": sh("free -m"),
        "numactl": sh("numactl -H"), "thp_enabled": sh("cat /sys/kernel/mm/transparent_hugepage/enabled"),
        "thp_defrag": sh("cat /sys/kernel/mm/transparent_hugepage/defrag"),
        "governor": sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "cxx": sh("${CXX:-g++} --version | head -1"), "deps": sh(f"cat {ROOT}/deps/VERSIONS"),
        "cxxflags": sh(f"make -s -C {ROOT} -pn 2>/dev/null | grep '^CXXFLAGS' | head -1"),
        "loadavg": sh("cat /proc/loadavg"), "nproc": os.cpu_count(),
        "disk": sh("df -h . && lsblk -d -o NAME,ROTA,MODEL,SIZE"),
    }


def numa_prefix(kind):
    """Single-threaded cells: pin to one core, and memory to its node when that works."""
    if kind != "single":
        return []
    if shutil.which("numactl") and subprocess.run("numactl --physcpubind=2 --membind=0 true", shell=True).returncode == 0:
        return ["numactl", "--physcpubind=2", "--membind=0"]
    if shutil.which("taskset") and subprocess.run("taskset -c 2 true", shell=True).returncode == 0:
        return ["taskset", "-c", "2"]
    sys.exit("cannot pin to a core: neither numactl nor taskset works here")


# --------------------------------------------------------------------------
# plans: each returns a list of cells {id, cmd(list), reps, kind, timeout, cwd}
# --------------------------------------------------------------------------
def B(name):
    return os.path.join(BUILD, name)


def cell(cid, cmd, reps, kind="single", timeout=7200, cwd=None, env=None):
    return {"id": cid, "cmd": [str(c) for c in cmd], "reps": reps, "kind": kind, "timeout": timeout, "cwd": cwd, "env": env}


# mode -> access patterns timed inside one process, after one build
# Every process gets huge pages: the array and the hand-written tables ask for them
# with madvise, and this makes malloc do the same for absl, Boost and unordered_map,
# which transparent huge pages in madvise mode would otherwise leave on 4 KiB pages.
ENV = {**os.environ, "GLIBC_TUNABLES": "glibc.malloc.hugetlb=1"}

H2H_MODES = {"rankonly": "ind", "rankonly+two": "ind",
             "tapered-ram": "ind,dep,batch", "tapered-ram+two": "ind,dep,batch", "tapered-ram+pre": "ind,dep",
             "tapered-ram-nohuge": "ind,dep", "tapered": "ind,dep,batch", "tapered+pin": "ind,dep",
             "tabled": "ind,dep", "oahash": "ind,dep,batch", "oahash-nohuge": "ind,dep",
             "absl": "ind,dep,batch", "boost": "ind,dep", "hash": "ind,dep"}


def plan_h2h(m, a):
    reps = 10 if m == "server" else 7
    return [cell(f"h2h/{x}", [B("bench"), x, 16, 13, 3000000, 42, p, "{rep}"], reps) for x, p in H2H_MODES.items()]


def plan_sweep(m, a):
    modes = ["tapered-ram", "tapered", "oahash", "absl", "boost", "hash"]
    if m == "server":
        Bs, M, reps = [16, 20, 24, 29, 32, 34], 10000000, 5
    else:
        Bs, M, reps = [10, 14, 18, 22], 2000000, 7
    cells = []
    for b in Bs:
        for x in modes:
            r = reps
            if m == "server" and x in ("hash", "boost", "absl", "oahash") and b >= 32:
                r = 3                                   # builds of 5-9 min each
            cells.append(cell(f"sweep/{x}/B{b}", [B("bench"), x, b, 10, M, 42, H2H_MODES[x], "{rep}"], r))
    return cells


def plan_pages(m, a):
    """Control: what the default page policy costs the malloc-backed tables."""
    default = {k: v for k, v in os.environ.items() if k != "GLIBC_TUNABLES"}
    return [cell(f"pages/{x}", [B("bench"), x, 16, 13, 3000000, 42, "ind,dep", "{rep}"], 5, env=default)
            for x in ("absl", "boost", "hash")]


def plan_window(m, a):
    """How overlap depends on d at about the same table size, and on the batch size."""
    cells = []
    for (b, d) in [(48, 6), (28, 8), (20, 10), (16, 13), (14, 15), (13, 16), (11, 20)]:
        for x in ["tapered-ram", "tapered-ram+two"] + (["oahash", "absl"] if d <= 15 else []):    # packed keys: 63 bits
            cells.append(cell(f"window/{x}/d{d}", [B("bench"), x, b, d, 3000000, 42, H2H_MODES[x], "{rep}"], 5))
    cells.append(cell("window/batchsize", [B("bench"), "tapered-ram", 16, 13, 3000000, 42,
                                           "batch:4,batch:8,batch:16,batch:32,batch:64,batch:128", "{rep}"], 5))
    return cells


def plan_parallel(m, a):
    Ts = [1, 2, 4, 8, 16, 32, 64, 128] if m == "server" else [1, 2, 4, 8]
    Ts = [t for t in Ts if t <= (os.cpu_count() or 1)]
    b, d = (32, 10) if m == "server" else (24, 10)
    cells = []
    # the page-fault question: index-only payload on every backing, with and without pre-faulting
    for storage in ["shm", "anon-4k", "anon-thp"]:
        for pf in [0, 1]:
            for t in Ts:
                cells.append(cell(f"par/array-index/{storage}/pf{pf}/B{b}d{d}/T{t}",
                                  [B("build_parallel"), "array-index", b, d, t, 42, "cheap", storage, pf], 3, "multi"))
    for mode in ["array-unrank", "array-odo"]:
        for t in Ts:
            cells.append(cell(f"par/{mode}/anon-thp/pf0/B{b}d{d}/T{t}",
                              [B("build_parallel"), mode, b, d, t, 42, "cheap", "anon-thp", 0], 3, "multi"))
    hts = [t for t in [1, 4, 16, 64, 128] if t <= (os.cpu_count() or 1)]
    for mode in ["umap-shard", "absl-shard", "boost-conc", "oa-cas"]:
        for t in hts:
            cells.append(cell(f"par/{mode}/B{b}d{d}/T{t}", [B("build_parallel"), mode, b, d, t, 42, "cheap"], 3, "multi"))
    # expensive payload: the vector root on the B=16, d=12 grid (11 stored)
    for mode in ["array-odo", "oa-cas", "absl-shard", "umap-shard"]:
        for t in [t for t in [1, 4, 16, 64] if t <= (os.cpu_count() or 1)]:
            cells.append(cell(f"par-pl/{mode}/T{t}", [B("build_parallel"), mode, 16, 11, t, 42, "pl"], 3, "multi"))
    return cells


PL_GRIDS = [(12, 8), (16, 12), (20, 12), (16, 16)]


def plan_pl(m, a):
    th = os.cpu_count() or 1
    os.makedirs(os.path.join(ROOT, "tables"), exist_ok=True)
    cells = []
    for (b, d) in PL_GRIDS:
        tab = os.path.join(ROOT, "tables", f"pl_B{b}_d{d}.tg")
        # first process builds+saves the table; on the server it also runs the
        # bit-identity check, whose absl memo does not fit beside a desktop session
        verify = 1 if m == "server" else 0
        cells.append(cell(f"pl/verify/B{b}d{d}", [B("pl_bench"), b, d, 2.0, 5, th, tab, verify, 5], 1, "multi", 14400))
        cells.append(cell(f"pl/lat/B{b}d{d}", [B("pl_bench"), b, d, 2.0, 5, th, tab, 0, 5], 3))
    return cells


def plan_plfit(m, a):
    th = os.cpu_count() or 1
    # the absl memo of the "hash" variant needs 5 GB: server only
    variants = "exact,nearest,simplex,newton1,snewton1,twostage" + (",hash" if m == "server" else "")
    cells = []
    for (b, d) in [(16, 12), (16, 16)]:
        tab = os.path.join(ROOT, "tables", f"pl_B{b}_d{d}.tg")
        cells.append(cell(f"plfit/B{b}d{d}", [B("pl_fit"), f"B={b}", f"d={d}", "n=20000", "R=20", f"threads={th}",
                                              f"variants={variants}", f"table={tab}"], 1, "single", 4 * 3600))
    return cells


def plan_entmax(m, a):
    rows = os.path.join(ROOT, "data", "entmax_u.bin")
    return [cell("entmax/B16d12", [B("entmax_bench"), 16, 12, 2.0, 5, rows], 3)]


def plan_ooc(m, a):
    od = a.ooc_dir
    f = os.path.join(od, "ooc_memo.tg")
    if m == "server":
        b, d = 32, 10                                   # 4.48 GB on NVMe
        return [cell("ooc/build", [B("ooc_bench"), "build", b, d, f], 1, "single", 3600),
                cell("ooc/query-cold", [B("ooc_bench"), "query", b, d, 20000, 42, f, 1], 3),
                cell("ooc/query-warm", [B("ooc_bench"), "query", b, d, 20000, 42, f, 0], 3)]
    b, d = 22, 15                                       # 22.27 GB on the HDD
    tr = os.path.join(od, "pl_trace.bin")
    WARM = -5000000                                     # see ooc_bench.cpp: steps generated, not replayed
    cells = [cell("ooc/build", [B("ooc_bench"), "build", b, d, f], 1, "single", 3600),
             cell("ooc/scan", [B("ooc_bench"), "scan", b, d, f], 1, "single", 3600),
             cell("ooc/query-cold", [B("ooc_bench"), "query", b, d, 20000, 42, f, 1], 3),
             cell("ooc/query-warmed", [B("ooc_bench"), "query", b, d, 20000, 42, f, 1, WARM], 3)]
    for addr in ["lex", "hashed"]:
        for seed in [42, 7, 11]:
            cells.append(cell(f"walk/uniform-cold/{addr}/s{seed}", [B("ooc_bench"), "walk", b, d, 20000, seed, addr, 0, f], 1))
            cells.append(cell(f"walk/uniform-warm/{addr}/s{seed}", [B("ooc_bench"), "walk", b, d, 20000, seed, addr, WARM, f], 1))
            cells.append(cell(f"walk/gauss0.25/{addr}/s{seed}", [B("ooc_bench"), "gauss", b, d, 20000, seed, addr, 0.25, 0, f], 1))
            cells.append(cell(f"walk/gausswarm0.25/{addr}/s{seed}", [B("ooc_bench"), "gauss", b, d, 20000, seed, addr, 0.25, WARM, f], 1))
            cells.append(cell(f"walk/gauss0.5/{addr}/s{seed}", [B("ooc_bench"), "gauss", b, d, 20000, seed, addr, 0.5, 0, f], 1))
        cells.append(cell(f"walk/pltrace/{addr}", [B("ooc_bench"), "trace", b, d, tr, addr, 0, f], 3))
    # the trace: a complete exact fit of a 1000-decision data set, snapped to the file's grid
    gen = cell("ooc/pltrace-gen", [B("pl_fit"), "B=8", "d=6", "n=1000", "R=1", "variants=exact",
                                   f"trace_out={tr}", "trace_B=22", "trace_d=16", "trace_max=100000"], 1)
    return cells[:4] + [gen] + cells[4:]


PLANS = {"h2h": plan_h2h, "pages": plan_pages, "window": plan_window, "sweep": plan_sweep, "parallel": plan_parallel, "pl": plan_pl,
         "plfit": plan_plfit, "entmax": plan_entmax, "ooc": plan_ooc}
ORDER = {"server": ["h2h", "pages", "window", "sweep", "parallel", "pl", "plfit", "entmax", "ooc"],
         "desktop": ["h2h", "pages", "window", "sweep", "pl", "plfit", "ooc"]}


def done_set(path):
    s = set()
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    o = json.loads(line)
                    if o.get("rc") == 0:
                        s.add((o["cell"], o["rep"]))
                except Exception:  # noqa: BLE001
                    pass
    return s


def run_plan(machine, plan, cells, a):
    out_dir = os.path.join(ROOT, "results", machine)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{plan}.jsonl")
    done = done_set(path)
    max_reps = max(c["reps"] for c in cells)
    for rep in range(max_reps):
        order = [c for c in cells if rep < c["reps"]]
        if plan not in ("ooc", "pl"):                   # these cells have a required order
            random.Random(rep).shuffle(order)           # (pl: verify builds the table first)
        for c in order:
            if (c["id"], rep) in done:
                continue
            if (machine == "desktop" and plan == "ooc" and not a.dry_run and not a.force
                    and mem_available_gib() < a.min_free_gib):
                print(f"STOP: MemAvailable {mem_available_gib():.1f} GiB < {a.min_free_gib} GiB; close VS Code/browser "
                      f"or pass --force", flush=True)
                sys.exit(3)
            cmd = numa_prefix(c["kind"]) + [x.replace("{rep}", str(rep)) for x in c["cmd"]]
            print(f"[{plan} rep {rep}] {' '.join(shlex.quote(x) for x in cmd)}", flush=True)
            if a.dry_run:
                continue
            la0 = open("/proc/loadavg").read().split()[:3]
            avail0 = mem_available_gib()
            t0 = time.time()
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=c["timeout"], cwd=c["cwd"] or ROOT, env=c["env"] or ENV)
                rc, out, err = p.returncode, p.stdout, p.stderr
            except subprocess.TimeoutExpired as e:
                so = e.stdout or b""
                rc, out, err = -9, so.decode(errors="replace") if isinstance(so, bytes) else so, "TIMEOUT"
            wall = time.time() - t0
            la1 = open("/proc/loadavg").read().split()[:3]
            meta = {"cell": c["id"], "rep": rep, "rc": rc, "cmd": cmd, "wall_s": round(wall, 3),
                    "load_before": la0, "load_after": la1, "mem_avail_gib": round(avail0, 2),
                    "machine": machine, "plan": plan}
            rows = [json.loads(l[5:]) for l in out.splitlines() if l.startswith("JSON ")]
            for r in rows:                              # a binary's own repetition index
                if "rep" in r:                          # must not overwrite the runner's
                    r["inner_rep"] = r.pop("rep")
            with open(path, "a") as f:
                if not rows:
                    f.write(json.dumps({**meta, "stdout": out[-2000:], "stderr": err[-2000:]}) + "\n")
                for r in rows:
                    f.write(json.dumps({**meta, **r}) + "\n")
            if rc != 0:
                print(f"   rc={rc} {err.strip()[-300:]}", flush=True)
            else:
                print(f"   ok {wall:.1f}s", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--machine", required=True, choices=["server", "desktop"])
    ap.add_argument("--plans", default="all", help="comma list of " + ",".join(PLANS) + " or 'all'")
    ap.add_argument("--ooc-dir", default=os.path.join(ROOT, "ooc"), help="directory for the out-of-core file")
    ap.add_argument("--min-free-gib", type=float, default=12.0)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--keep-ooc", action="store_true", help="do not delete the out-of-core file at the end")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    plans = ORDER[a.machine] if a.plans == "all" else a.plans.split(",")
    os.makedirs(a.ooc_dir, exist_ok=True)
    out_dir = os.path.join(ROOT, "results", a.machine)
    os.makedirs(out_dir, exist_ok=True)
    missing = [b for b in ("bench", "build_parallel", "ooc_bench", "pl_bench", "pl_fit", "entmax_bench", "test_tg")
               if not os.path.exists(B(b))]
    if missing:
        print("missing binaries " + ", ".join(missing) + ": run scripts/get_deps.sh && make first"); sys.exit(2)
    py = "uv run --no-project python" if shutil.which("uv") else "python3"
    if not a.dry_run:
        with open(os.path.join(out_dir, f"env-{time.strftime('%Y%m%d-%H%M%S')}.json"), "w") as f:
            json.dump(env_record(a.machine), f, indent=1)
        v = subprocess.run(["make", "-s", "-C", ROOT, "verify", f"PY={py}"], capture_output=True, text=True)
        with open(os.path.join(out_dir, "verify.log"), "w") as f:
            f.write(v.stdout + v.stderr)
        print(v.stdout[-1500:])
        if v.returncode != 0:
            print("VERIFY FAILED -- not measuring anything. See results/<machine>/verify.log"); sys.exit(2)
        shutil.copy(os.path.join(BUILD, "penalty.log"), os.path.join(out_dir, "penalty.log"))
        ls = subprocess.run([sys.executable, os.path.join(ROOT, "analysis", "loopstats.py")], capture_output=True, text=True)
        with open(os.path.join(out_dir, "loopstats.json"), "w") as f:
            f.write(ls.stdout)
    for p in plans:
        run_plan(a.machine, p, PLANS[p](a.machine, a), a)
    if "ooc" in plans and not a.keep_ooc and not a.dry_run:
        for fn in ("ooc_memo.tg",):
            fp = os.path.join(a.ooc_dir, fn)
            if os.path.exists(fp):
                os.remove(fp)
                print(f"removed {fp}")
    print("ALL PLANS DONE")


if __name__ == "__main__":
    main()
