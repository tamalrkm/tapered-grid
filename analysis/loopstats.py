#!/usr/bin/env python3
"""Instructions and memory reads per coordinate of the two rank loops.

Compiles src/rank_forms.cpp with the benchmarks' flags, disassembles it, and
counts the body of each function's one backward branch. Prints one JSON line,
which scripts/run.py stores as results/<machine>/loopstats.json.
"""
import json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAGS = (os.environ.get("CXXFLAGS") or "-O3 -march=native -ffp-contract=off").split() + ["-std=c++17", "-pthread"]


def body(lines, fn):
    i = next(k for k, l in enumerate(lines) if f"<{fn}" in l and l.rstrip().endswith(">:"))
    code = []
    for l in lines[i + 1:]:
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", l)
        if not m:
            break
        code.append((int(m[1], 16), m[2].strip()))
    for addr, ins in code:                     # the loop: a branch back to a lower address
        m = re.match(r"j\w+\s+([0-9a-f]+)\b", ins)
        if m and int(m[1], 16) < addr:
            loop = [x for a, x in code if int(m[1], 16) <= a <= addr and not x.startswith(("nop", "data16", "cs nop"))]
            reads = sum(1 for x in loop if "(" in x and not x.startswith(("lea", "j")))
            return len(loop), reads
    raise SystemExit(f"no loop in {fn}")


def main():
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, "forms.o")
        subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-I", os.path.join(ROOT, "src"), "-c",
                        os.path.join(ROOT, "src", "rank_forms.cpp"), "-o", o], check=True)
        txt = subprocess.run(["objdump", "-d", "--no-show-raw-insn", "-C", o], capture_output=True, text=True, check=True).stdout
    lines = txt.splitlines()
    one, two = body(lines, "rank_one_read"), body(lines, "rank_two_read")
    print(json.dumps({"one_read_instr": one[0], "one_read_loads": one[1], "two_read_instr": two[0], "two_read_loads": two[1]}))


if __name__ == "__main__":
    sys.exit(main())
