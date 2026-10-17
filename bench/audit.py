#!/usr/bin/env python3
"""Counts how many hosts of a topology file are listed in a Slurm topology.conf produced by ibnetlens and by
ibtopology, and how many sit under a leaf switch that has no parent. ibtopology is run with 50 different hash seeds
because its output varies from run to run.

usage: audit.py FILE.topo [FILE.topo ...]   (run bench/fetch-analogs.sh first)
"""
import csv
import os
import subprocess
import sys

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
sys.path.insert(0, os.path.join(BENCH, ".analogs", "pylib"))
import hostlist  # noqa: E402

IBNETLENS = os.path.join(ROOT, "ibnetlens")
KOSTTY = os.path.join(BENCH, ".analogs", "ibtopology3.py")


def parse_conf(text):
    switches = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line.startswith("SwitchName="):
            continue
        fields = dict(f.split("=", 1) for f in line.split() if "=" in f)
        switches[fields["SwitchName"]] = (
            hostlist.expand_hostlist(fields["Switches"]) if "Switches" in fields else [],
            hostlist.expand_hostlist(fields["Nodes"]) if "Nodes" in fields else [],
        )
    return switches


def analyse(switches):
    """Returns (roots, hosts in the file, hosts whose leaf switch hangs under no other switch)."""
    children = {c for kids, _ in switches.values() for c in kids}
    roots = [s for s in switches if s not in children]
    hosts = {h for _, nodes in switches.values() for h in nodes}
    has_spines = any(kids for kids, _ in switches.values())
    detached = set()
    if has_spines:
        for r in roots:
            kids, nodes = switches[r]
            if not kids:
                detached.update(nodes)
    return roots, hosts, detached


def host_names(path):
    out = subprocess.run([IBNETLENS, "-F", "csv-nodes", path], capture_output=True, text=True, check=True).stdout
    names = set()
    for r in csv.DictReader(out.splitlines()):
        if r["type"] == "ca" and r["desc"].split():
            names.add(r["desc"].split()[0])
    return names


SEEDS = range(1, 51)  # ibtopology's output depends on Python's string hash seed


def main():
    base_env = dict(os.environ, PYTHONPATH=os.path.join(BENCH, ".analogs", "pylib"), PYTHONWARNINGS="ignore")
    print(f"{'input':<22} {'hosts':>6} | {'tool':<11} {'roots':>5} {'listed':>6} {'missing':>7} {'detached':>10}")
    for path in sys.argv[1:]:
        total = host_names(path)
        r = subprocess.run([IBNETLENS, "-F", "slurm", path], capture_output=True, text=True)
        roots, hosts, detached = analyse(parse_conf(r.stdout))
        print(f"{os.path.basename(path):<22} {len(total):>6} | {'ibnetlens':<11} {len(roots):>5} "
              f"{len(total & hosts):>6} {len(total - hosts):>7} {len(detached):>10}")

        results = {}
        failed = None
        for seed in SEEDS:
            env = dict(base_env, PYTHONHASHSEED=str(seed))
            r = subprocess.run(["python3", KOSTTY, "-f", path], capture_output=True, text=True, env=env)
            roots, hosts, detached = analyse(parse_conf(r.stdout))
            if r.returncode != 0 or not hosts:
                failed = (r.returncode, (r.stderr.strip().splitlines() or ["no output"])[-1][:100])
            key = (len(roots), len(total & hosts), len(total - hosts), len(detached))
            results[key] = results.get(key, 0) + 1
        for (nroots, listed, missing, det), count in sorted(results.items(), key=lambda kv: -kv[1]):
            print(f"{os.path.basename(path):<22} {len(total):>6} | {'ibtopology':<11} {nroots:>5} {listed:>6} {missing:>7} "
                  f"{det:>10}   ({count} of {len(SEEDS)} runs)")
        if failed:
            print(f"  ibtopology: exit {failed[0]}: {failed[1]}")


if __name__ == "__main__":
    main()
