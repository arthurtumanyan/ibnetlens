#!/usr/bin/env python3
"""Benchmarks ibnetlens on generated fat-trees and, when present, against Python tools.

Uses only the Python standard library. Writes bench.json into --out.

usage: bench/run.py --out DIR [--quick] [--repeat N] [--ibnetlens PATH] [--fattree PATH]
Python baselines: bench/ref_parse.py always; ibtopology after bench/fetch-analogs.sh.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
WORK = os.path.join(BENCH, ".work")
KOSTTY = os.path.join(BENCH, ".analogs", "ibtopology3.py")
PYLIB = os.path.join(BENCH, ".analogs", "pylib")
HOSTS = 32
SPINES = 32
TIMEOUT = 600
STATS = re.compile(r"(\w+)=([0-9.]+)")


def run_timed(cmd, env=None, repeat=1):
    """Best wall time over `repeat` runs, or None on failure or timeout."""
    best = None
    for _ in range(repeat):
        start = time.perf_counter()
        # wait() blocks in the kernel; subprocess.run(timeout=...) polls with up to 50 ms steps
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        killer = threading.Timer(TIMEOUT, proc.kill)
        killer.start()
        rc = proc.wait()
        wall = time.perf_counter() - start
        killer.cancel()
        if rc not in (0, 1):
            return None
        best = wall if best is None else min(best, wall)
    return best


def run_interleaved(cmds, repeat):
    """Best wall time per command, alternating the commands so that CPU frequency and core placement
    effects (performance vs efficiency cores) hit all of them alike."""
    best = [None] * len(cmds)
    for _ in range(repeat):
        for i, cmd in enumerate(cmds):
            t = run_timed(cmd)
            if t is not None and (best[i] is None or t < best[i]):
                best[i] = t
    return best


def ibnetlens_peak_mb(ibnetlens, args):
    """Peak memory as reported by ibnetlens itself (--stats), in MB."""
    r = subprocess.run([ibnetlens] + args + ["--stats"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    for line in r.stderr.splitlines():
        if "stats" in line:
            return int(dict(STATS.findall(line))["peak_kb"]) / 1024
    return None


def external_peak_mb(cmd, env=None):
    """Peak memory of a non-ibnetlens command via GNU time, when available."""
    tool = "/usr/bin/time"
    if not os.path.exists(tool):
        return None
    try:
        r = subprocess.run([tool, "-f", "%M"] + cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                           env=env, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    try:
        return int(r.stderr.strip().splitlines()[-1]) / 1024
    except (ValueError, IndexError):
        return None


def generate(fattree, leaves):
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "ft-%d.topo" % leaves)
    if not os.path.exists(path):
        with open(path, "w") as fh:
            subprocess.run([fattree, str(leaves), str(SPINES), str(HOSTS)], stdout=fh, check=True)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--repeat", type=int, default=0)
    ap.add_argument("--ibnetlens", default=os.path.join(ROOT, "ibnetlens"))
    ap.add_argument("--fattree", default=os.path.join(ROOT, "build", "fattree"))
    a = ap.parse_args()

    leaves_list = [8, 64, 512] if a.quick else [8, 32, 128, 512, 1024, 2048, 4096]
    have_kostty = os.path.exists(KOSTTY)
    kenv = dict(os.environ, PYTHONPATH=PYLIB, PYTHONWARNINGS="ignore")
    ref = os.path.join(BENCH, "ref_parse.py")
    py = sys.executable or "python3"
    rows = []

    for leaves in leaves_list:
        path = generate(a.fattree, leaves)
        size_mb = os.path.getsize(path) / 1e6
        rep = a.repeat or (3 if a.quick else (7 if size_mb < 5 else 5))
        row = {"leaves": leaves, "nodes": leaves * (HOSTS + 1) + SPINES, "size_mb": round(size_mb, 2)}
        row["ibnetlens_j1_s"], row["ibnetlens_auto_s"] = run_interleaved(
            [[a.ibnetlens, "info", "-q", "--no-progress", "-j", "1", path], [a.ibnetlens, "info", "-q", "--no-progress", path]],
            rep * 2)
        row["ibnetlens_j1_mb"] = ibnetlens_peak_mb(a.ibnetlens, ["info", "-q", "--no-progress", "-j", "1", path])
        row["ibnetlens_auto_mb"] = ibnetlens_peak_mb(a.ibnetlens, ["info", "-q", "--no-progress", path])
        row["ibnetlens_slurm_s"] = run_timed([a.ibnetlens, "-F", "slurm", "--no-progress", path], repeat=rep)
        row["ibnetlens_check_s"] = run_timed([a.ibnetlens, "check", "--no-progress", path], repeat=rep)
        row["python_s"] = run_timed([py, ref, path], repeat=rep)
        row["python_mb"] = external_peak_mb([py, ref, path])
        if have_kostty:
            row["ibtopology_s"] = run_timed([py, KOSTTY, "-f", path], env=kenv)
            row["ibtopology_mb"] = external_peak_mb([py, KOSTTY, "-f", path], env=kenv)
        rows.append(row)
        print("bench: %5d leaves %7.1f MB  ibnetlens %.3fs (1 thread) %.3fs (auto)  python %.3fs" % (
            leaves, size_mb, row["ibnetlens_j1_s"] or -1, row["ibnetlens_auto_s"] or -1, row["python_s"] or -1), flush=True)

    big = generate(a.fattree, leaves_list[-1])
    cpus = os.cpu_count() or 1
    counts = sorted({1, 2, 4, 8, 12, 16, 24, 32, cpus} & set(range(1, cpus + 1)) | {1})
    threads = []
    for j in counts:
        t = run_timed([a.ibnetlens, "info", "-q", "--no-progress", "-j", str(j), big], repeat=3)
        threads.append({"threads": j, "seconds": t})
        print("bench: threads %2d -> %.3fs" % (j, t or -1), flush=True)

    tiny = generate(a.fattree, 8)
    startup = {"ibnetlens_s": run_timed([a.ibnetlens, "info", "-q", "--no-progress", tiny], repeat=30),
               "python_s": run_timed([py, ref, tiny], repeat=30)}

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "bench.json"), "w") as fh:
        json.dump({"hosts_per_leaf": HOSTS, "spines": SPINES, "scaling": rows, "threads": threads,
                   "startup": startup, "analogs": have_kostty}, fh, indent=1)
    print("bench: wrote", os.path.join(a.out, "bench.json"))


if __name__ == "__main__":
    main()
