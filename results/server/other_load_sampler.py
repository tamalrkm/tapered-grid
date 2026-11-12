#!/usr/bin/env python3
"""Every 60 s: host-wide busy cores (from /proc/stat) minus the cores used by the
runner's process tree = cores used by everything else (other tenants, other apps).
Read-only; stops when the runner pid exits.  usage: sampler.py RUNNER_PID OUT.tsv"""
import os, sys, time

HZ = os.sysconf("SC_CLK_TCK")
root = int(sys.argv[1]); out = sys.argv[2]


def host():
    v = list(map(int, open("/proc/stat").readline().split()[1:]))
    total = sum(v[:8]); idle = v[3] + v[4]
    return total - idle, total


def tree_cpu():
    kids, cpu = {}, {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            s = open(f"/proc/{p}/stat").read()
        except OSError:
            continue
        f = s[s.rindex(")") + 2:].split()
        kids.setdefault(int(f[1]), []).append(int(p))
        cpu[int(p)] = sum(map(int, f[11:15]))      # utime stime cutime cstime
    tot, st = 0, [root]
    while st:
        q = st.pop(); tot += cpu.get(q, 0); st += kids.get(q, [])
    return tot


new = not os.path.exists(out)
with open(out, "a") as fo:
    if new:
        fo.write("utc\tload1\tload5\thost_busy_cores\trunner_cores\tother_cores\n")
    b0, t0 = host(); r0 = tree_cpu()
    while os.path.exists(f"/proc/{root}"):
        time.sleep(60)
        b1, t1 = host(); r1 = tree_cpu()
        ncpu = os.cpu_count()
        span = (t1 - t0) / ncpu / HZ                    # seconds elapsed
        hb = (b1 - b0) / HZ / span; rb = max(0, r1 - r0) / HZ / span
        la = open("/proc/loadavg").read().split()
        fo.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\t{la[0]}\t{la[1]}\t{hb:.2f}\t{rb:.2f}\t{hb - rb:.2f}\n")
        fo.flush()
        b0, t0, r0 = b1, t1, r1
