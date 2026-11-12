"""Shared loader: rows of results/<machine>/<plan>.jsonl, and robust summaries."""
import json, os, statistics as st
from collections import defaultdict

def rows(root, machine, plan):
    p = os.path.join(root, "results", machine, f"{plan}.jsonl")
    if not os.path.exists(p):
        return []
    rs = [json.loads(l) for l in open(p) if l.strip()]
    return [r for r in rs if r.get("rc", 0) == 0]

def q(xs, f):
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * f; i = int(k); j = min(i + 1, len(xs) - 1)
    return xs[i] + (xs[j] - xs[i]) * (k - i)

def summ(xs):
    return {"n": len(xs), "med": st.median(xs), "q1": q(xs, .25), "q3": q(xs, .75), "min": min(xs), "max": max(xs)}

def group(rs, key, val):
    g = defaultdict(list)
    for r in rs:
        if val in r and r.get("rc", 0) == 0:
            g[key(r)].append(r[val])
    return {k: summ(v) for k, v in g.items()}
