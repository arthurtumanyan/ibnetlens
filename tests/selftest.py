#!/usr/bin/env python3
"""One-command test run for any machine. Uses only the Python standard library.

Builds ibnetlens if needed, runs every test suite that can run here, optionally the benchmark,
writes machine.json / tests.json / bench.json and an HTML report into the output directory.

usage: tests/selftest.py [--tag NAME] [--out DIR] [--bench quick|full] [--sanitizers] [--samples] [--large]
                         [--bin PATH] [--no-build] [--with-hostname]
Environment: CC selects the compiler; IBPREFIX / SLURMPREFIX / NVPREFIX point at unpacked ibsim and
infiniband-diags, slurmctld, and NVIDIA ibutils2 packages when they are not installed system-wide
(tests/fetch-nvidia.sh downloads the NVIDIA ones).
Exit status: 0 when no test failed, 1 otherwise.
"""
import argparse
import datetime
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINE = re.compile(r"^(ok|FAIL|skip)\s+(.*)$")


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=ROOT, **kw)


def read_first(path, pattern):
    try:
        with open(path) as fh:
            for line in fh:
                if re.match(pattern, line):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return ""


def os_name():
    try:
        with open("/etc/os-release") as fh:
            for line in fh:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return platform.platform()


def gpu_names():
    if not shutil.which("lspci"):
        return ""
    r = sh(["lspci"])
    names = [l.split(": ", 1)[-1] for l in r.stdout.splitlines() if re.search(r"VGA|3D controller|Display", l)]
    return "; ".join(names)


def machine_info(binary, tag, with_hostname):
    cc = os.environ.get("CC", "cc")
    compiler = sh([cc, "--version"]).stdout.splitlines()[:1]
    version = sh([binary, "--version"]).stdout.strip()
    simd = "?"
    r = sh([binary, "info", "-q", "--no-progress", "--stats", os.path.join(ROOT, "tests", "golden", "ft-small.topo")])
    m = re.search(r"simd=(\d)", r.stdout)
    if m:
        simd = "yes" if m.group(1) == "1" else "no (scalar)"
    commit = ""
    if os.path.isdir(os.path.join(ROOT, ".git")) and shutil.which("git"):
        commit = sh(["git", "describe", "--always", "--dirty"]).stdout.strip()
    mem = read_first("/proc/meminfo", r"MemTotal")
    info = {
        "tag": tag,
        "cpu": read_first("/proc/cpuinfo", r"model name") or read_first("/proc/cpuinfo", r"Hardware") or platform.processor(),
        "cpus": os.cpu_count(),
        "memory_mb": int(mem.split()[0]) // 1024 if mem else "",
        "os": os_name(),
        "kernel": platform.release(),
        "arch": platform.machine(),
        "compiler": compiler[0] if compiler else cc,
        "version": version,
        "commit": commit,
        "simd": simd,
        "gpu": gpu_names(),
        "python": platform.python_version(),
        "date": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
    }
    if with_hostname:
        info["hostname"] = socket.gethostname()
    return info


def run_suite(name, cmd, results, env=None):
    start = time.time()
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=ROOT,
                       env=dict(os.environ, **(env or {})))
    found = 0
    for line in r.stdout.splitlines():
        m = LINE.match(line)
        if m:
            results.append({"suite": name, "status": m.group(1), "name": m.group(2)})
            found += 1
    if "unit tests passed" in r.stdout:
        results.append({"suite": name, "status": "ok", "name": "unit tests passed"})
        found += 1
    failed = any(x["status"] == "FAIL" and x["suite"] == name for x in results)
    if r.returncode != 0 and not failed:
        results.append({"suite": name, "status": "FAIL", "name": "exited with status %d" % r.returncode})
        failed = True
    if failed:
        tail = "\n".join(r.stdout.splitlines()[-25:])
        print("--- %s output (tail) ---\n%s\n---" % (name, tail))
    if found == 0 and r.returncode == 0:
        results.append({"suite": name, "status": "skip", "name": "no results reported"})
    print("%-22s %.1fs  %s" % (name, time.time() - start, "ok" if r.returncode == 0 else "FAILED"), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default=None, help="label for this machine in the report")
    ap.add_argument("--out", default=None)
    ap.add_argument("--bench", choices=["quick", "full"], default=None)
    ap.add_argument("--sanitizers", action="store_true", help="also run the ASan/UBSan and TSan suites")
    ap.add_argument("--large", action="store_true", help="also cross-check fabrics of up to 61k nodes against the real ibnetdiscover (slow)")
    ap.add_argument("--samples", action="store_true", help="download and test public sample dumps (needs network)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "ibnetlens"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--with-hostname", action="store_true", help="record the host name in machine.json")
    a = ap.parse_args()

    tag = a.tag or re.sub(r"[^A-Za-z0-9._-]+", "-", platform.node() or "machine")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = a.out or os.path.join(ROOT, "results", "%s-%s" % (tag, stamp))
    os.makedirs(out, exist_ok=True)
    if not a.bin or not os.path.isfile(a.bin):
        print("ibnetlens binary not found: %r" % a.bin)
        sys.exit(2)
    binary = os.path.abspath(a.bin)
    fattree = os.path.join(ROOT, "build", "fattree")
    started = time.time()

    if not a.no_build:
        print("building ...", flush=True)
        r = sh(["make", "ibnetlens", "build/fattree", "build/test_core"])
        if r.returncode != 0:
            print(r.stdout)
            sys.exit(2)
    if not os.path.exists(binary):
        print("ibnetlens binary not found: %s" % binary)
        sys.exit(2)

    machine = machine_info(binary, tag, a.with_hostname)
    with open(os.path.join(out, "machine.json"), "w") as fh:
        json.dump(machine, fh, indent=1)
    print("machine: %s, %s CPUs, %s" % (machine["cpu"], machine["cpus"], machine["os"]))

    results = []
    run_suite("golden and behaviour", [os.path.join(ROOT, "tests", "run.sh"), binary, fattree], results)
    run_suite("unit", [os.path.join(ROOT, "build", "test_core")], results)
    run_suite("real ibnetdiscover", [os.path.join(ROOT, "tests", "xcheck-ibnetdiscover.sh"), binary, fattree], results,
              env={"XCHECK_LARGE": "1"} if a.large else None)
    run_suite("NVIDIA ibdiagnet and ibtopodiff", [os.path.join(ROOT, "tests", "xcheck-nvidia.sh"), binary, fattree], results,
              env={"XCHECK_LARGE": "1"} if a.large else None)
    golden = os.path.join(ROOT, "tests", "golden")
    fixtures = [os.path.join(golden, f) for f in ("ft-medium.topo", "variety.topo", "mixed-spine.net", "ibsim.net")]
    run_suite("real slurmctld", [os.path.join(ROOT, "tests", "xcheck-slurm.sh"), binary] + fixtures, results)
    if a.samples:
        run_suite("public sample dumps", [os.path.join(ROOT, "tests", "fetch-samples.sh"), binary], results)
    if a.sanitizers:
        run_suite("AddressSanitizer", ["make", "asan"], results)
        run_suite("ThreadSanitizer", ["make", "tsan"], results)

    summary = {k: sum(1 for r in results if r["status"] == s) for k, s in (("ok", "ok"), ("fail", "FAIL"), ("skip", "skip"))}
    with open(os.path.join(out, "tests.json"), "w") as fh:
        json.dump({"summary": summary, "seconds": round(time.time() - started, 1), "results": results}, fh, indent=1)

    if a.bench:
        cmd = [sys.executable, os.path.join(ROOT, "bench", "run.py"), "--out", out, "--ibnetlens", binary, "--fattree", fattree]
        if a.bench == "quick":
            cmd.append("--quick")
        subprocess.run(cmd, cwd=ROOT)

    subprocess.run([sys.executable, os.path.join(ROOT, "bench", "report.py"), out], cwd=ROOT)
    print("\n%(ok)d passed, %(fail)d failed, %(skip)d skipped" % summary)
    print("results: %s" % out)
    print("report:  %s" % os.path.join(out, "report.html"))
    sys.exit(1 if summary["fail"] else 0)


if __name__ == "__main__":
    main()
