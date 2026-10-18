#!/usr/bin/env python3
"""Builds a self-contained HTML report (inline SVG charts) from one or more result directories
written by tests/selftest.py and bench/run.py. Uses only the Python standard library.

usage: bench/report.py DIR [DIR ...] [-o report.html] [--svg-dir DIR]
With several directories the charts overlay the machines so they can be compared.
"""
import argparse
import html
import json
import math
import os
import sys

PALETTE = ["#1f6feb", "#d9822b", "#2da44e", "#b3261e", "#8250df", "#0a7e8c"]
TOOLS = [  # key in bench rows, label, dash, marker
    ("ibnetlens_auto_s", "ibnetlens, all cores", "", "square"),
    ("ibnetlens_j1_s", "ibnetlens, 1 thread", "", "circle"),
    ("python_s", "Python regex parser", "6 4", "triangle"),
]
SLURM_TOOLS = [
    ("ibnetlens_slurm_s", "ibnetlens -F slurm", "", "circle"),
    ("ibtopology_s", "ibtopology (Python)", "6 4", "diamond"),
]
MEM_TOOLS = [
    ("ibnetlens_auto_mb", "ibnetlens, all cores", "", "square"),
    ("ibnetlens_j1_mb", "ibnetlens, 1 thread", "", "circle"),
    ("python_mb", "Python regex parser", "6 4", "triangle"),
]

SVG_STYLE = """
.bg{fill:#fff}.txt{fill:#24292f;font:12px system-ui,sans-serif}.title{fill:#24292f;font:600 14px system-ui,sans-serif}
.axis{stroke:#57606a;fill:none}.grid{stroke:#d0d7de;stroke-width:1}.minor{stroke:#eaeef2;stroke-width:1}
@media (prefers-color-scheme: dark){.bg{fill:#0d1117}.txt,.title{fill:#e6edf3}.axis{stroke:#8b949e}.grid{stroke:#30363d}.minor{stroke:#21262d}}
"""


def fmt_seconds(v):
    if v < 1e-3:
        return "%.0f µs" % (v * 1e6)
    if v < 1:
        return "%.3g ms" % (v * 1e3)
    return "%.3g s" % v


def fmt_count(v):
    if v >= 1e6:
        return "%gM" % (v / 1e6)
    if v >= 1e3:
        return "%gk" % (v / 1e3)
    return "%g" % v


def fmt_mb(v):
    return "%.3g MB" % v if v < 1000 else "%.3g GB" % (v / 1000)


def fmt_plain(v):
    return "%g" % v


def log_ticks(lo, hi):
    a, b = math.floor(math.log10(lo)), math.ceil(math.log10(hi))
    major = [10.0 ** e for e in range(a, b + 1)]
    minor = [m * 10.0 ** e for e in range(a, b) for m in range(2, 10)]
    return major, minor, 10.0 ** a, 10.0 ** b


def lin_ticks(lo, hi, n=5):
    span = hi - lo or 1.0
    step = 10 ** math.floor(math.log10(span / n))
    for m in (1, 2, 5, 10):
        if span / (step * m) <= n:
            step *= m
            break
    first = math.floor(lo / step) * step
    ticks = []
    t = first
    while t <= hi + step * 0.5:
        ticks.append(t)
        t += step
    return ticks, ticks[0], ticks[-1]


def marker(shape, x, y, color):
    if shape == "square":
        return '<rect x="%.1f" y="%.1f" width="8" height="8" fill="%s"/>' % (x - 4, y - 4, color)
    if shape == "triangle":
        return '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="%s"/>' % (x, y - 5, x - 5, y + 4, x + 5, y + 4, color)
    if shape == "diamond":
        return '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="%s"/>' % (
            x, y - 5, x + 5, y, x, y + 5, x - 5, y, color)
    return '<circle cx="%.1f" cy="%.1f" r="4" fill="%s"/>' % (x, y, color)


def line_chart(title, series, xlabel, ylabel, xfmt, yfmt, xlog=True, ylog=True, standalone=False, note=""):
    """series: list of dicts(label, color, dash, shape, points=[(x, y)])."""
    series = [s for s in series if s["points"]]
    if not series:
        return ""
    w = 760
    legend_rows = (len(series) + 1) // 2
    h = 440 + 6 + legend_rows * 18 + 22
    ml, mr, mt = 74, 24, 40
    pw, ph = w - ml - mr, 440 - mt - 60
    xs = [p[0] for s in series for p in s["points"]]
    ys = [p[1] for s in series for p in s["points"]]

    if xlog:
        xmaj, xmin, x0, x1 = log_ticks(min(xs), max(xs))
    else:
        xmaj, x0, x1 = lin_ticks(min(xs), max(xs))
        xmin = []
    if ylog:
        ymaj, ymin, y0, y1 = log_ticks(min(ys), max(ys))
    else:
        ymaj, y0, y1 = lin_ticks(0, max(ys))
        ymin = []

    def sx(v):
        t = (math.log10(v) - math.log10(x0)) / ((math.log10(x1) - math.log10(x0)) or 1) if xlog else (v - x0) / ((x1 - x0) or 1)
        return ml + t * pw

    def sy(v):
        t = (math.log10(v) - math.log10(y0)) / ((math.log10(y1) - math.log10(y0)) or 1) if ylog else (v - y0) / ((y1 - y0) or 1)
        return mt + ph - t * ph

    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" role="img" aria-label="%s">' % (w, h, html.escape(title)))
    if standalone:
        out.append("<style>%s</style>" % SVG_STYLE)
        out.append('<rect class="bg" width="%d" height="%d"/>' % (w, h))
    out.append('<text class="title" x="%d" y="22">%s</text>' % (ml, html.escape(title)))
    for v in xmin:
        out.append('<line class="minor" x1="%.1f" y1="%d" x2="%.1f" y2="%d"/>' % (sx(v), mt, sx(v), mt + ph))
    for v in ymin:
        out.append('<line class="minor" x1="%d" y1="%.1f" x2="%d" y2="%.1f"/>' % (ml, sy(v), ml + pw, sy(v)))
    for v in xmaj:
        out.append('<line class="grid" x1="%.1f" y1="%d" x2="%.1f" y2="%d"/>' % (sx(v), mt, sx(v), mt + ph))
        out.append('<text class="txt" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (sx(v), mt + ph + 18, xfmt(v)))
    for v in ymaj:
        out.append('<line class="grid" x1="%d" y1="%.1f" x2="%d" y2="%.1f"/>' % (ml, sy(v), ml + pw, sy(v)))
        out.append('<text class="txt" x="%d" y="%.1f" text-anchor="end">%s</text>' % (ml - 8, sy(v) + 4, yfmt(v)))
    out.append('<rect class="axis" x="%d" y="%d" width="%d" height="%d"/>' % (ml, mt, pw, ph))
    out.append('<text class="txt" x="%.1f" y="%d" text-anchor="middle">%s</text>' % (ml + pw / 2, mt + ph + 40, html.escape(xlabel)))
    out.append('<text class="txt" transform="translate(16,%.1f) rotate(-90)" text-anchor="middle">%s</text>' % (mt + ph / 2, html.escape(ylabel)))

    for s in series:
        pts = sorted(s["points"])
        coords = " ".join("%.1f,%.1f" % (sx(x), sy(y)) for x, y in pts)
        dash = ' stroke-dasharray="%s"' % s["dash"] if s["dash"] else ""
        out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2"%s/>' % (coords, s["color"], dash))
        for x, y in pts:
            out.append(marker(s["shape"], sx(x), sy(y), s["color"]))

    ly = mt + ph + 66
    for i, s in enumerate(series):
        col, row = i % 2, i // 2
        lx = ml + col * (pw / 2)
        y = ly + row * 18
        dash = ' stroke-dasharray="%s"' % s["dash"] if s["dash"] else ""
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="2"%s/>' % (lx, y, lx + 26, y, s["color"], dash))
        out.append(marker(s["shape"], lx + 13, y, s["color"]))
        out.append('<text class="txt" x="%.1f" y="%d">%s</text>' % (lx + 34, y + 4, html.escape(s["label"])))
    if note:
        out.append('<text class="txt" x="%d" y="%d" font-size="10" opacity="0.7">%s</text>' % (ml, h - 8, html.escape(note)))
    out.append("</svg>")
    return "\n".join(out)


def load_run(path):
    run = {"dir": path, "machine": {}, "tests": None, "bench": None}
    for key in ("machine", "tests", "bench"):
        f = os.path.join(path, key + ".json")
        if os.path.exists(f):
            with open(f) as fh:
                run[key] = json.load(fh)
    run["label"] = run["machine"].get("tag") or os.path.basename(os.path.normpath(path))
    return run


def build_series(runs, tools, key_x, value_ok=lambda v: v is not None):
    series = []
    for ri, run in enumerate(runs):
        if not run["bench"]:
            continue
        color = PALETTE[ri % len(PALETTE)]
        for key, label, dash, shape in tools:
            pts = [(r[key_x], r[key]) for r in run["bench"]["scaling"] if r.get(key) is not None]
            name = label if len(runs) == 1 else "%s - %s" % (label, run["label"])
            if len(runs) == 1:
                color = {"square": "#0a3d91", "circle": "#1f6feb", "triangle": "#d9822b", "diamond": "#b3261e"}[shape]
            series.append({"label": name, "color": color, "dash": dash, "shape": shape, "points": pts})
    return series


def charts(runs, standalone=False):
    note = "generated two-level fat-trees, %d hosts per leaf" % next((r["bench"]["hosts_per_leaf"] for r in runs if r["bench"]), 32)
    out = {}
    out["parse-time"] = line_chart("Parse and resolve links: time vs fabric size", build_series(runs, TOOLS, "nodes"),
                                   "nodes in the fabric", "seconds (lower is better)", fmt_count, fmt_seconds,
                                   standalone=standalone, note=note)
    out["slurm-time"] = line_chart("Slurm topology.conf generation: time vs fabric size",
                                   build_series(runs, SLURM_TOOLS, "nodes"), "nodes in the fabric",
                                   "seconds (lower is better)", fmt_count, fmt_seconds, standalone=standalone, note=note)
    out["memory"] = line_chart("Peak memory vs fabric size", build_series(runs, MEM_TOOLS, "nodes"),
                               "nodes in the fabric", "peak RSS (lower is better)", fmt_count, fmt_mb,
                               standalone=standalone, note=note)
    thr = []
    for ri, run in enumerate(runs):
        if not run["bench"]:
            continue
        pts = [(t["threads"], t["seconds"]) for t in run["bench"]["threads"] if t["seconds"]]
        if not pts:
            continue
        color = PALETTE[ri % len(PALETTE)]
        thr.append({"label": "ibnetlens" if len(runs) == 1 else "ibnetlens - " + run["label"], "color": color, "dash": "",
                    "shape": "square", "points": pts})
        base = min(pts)[1]
        thr.append({"label": "ideal scaling" if len(runs) == 1 else "ideal - " + run["label"], "color": "#8c959f",
                    "dash": "2 4", "shape": "circle", "points": [(x, base / x) for x, _ in pts]})
    out["threads"] = line_chart("Thread scaling on the largest input", thr, "threads (-j)", "seconds (lower is better)",
                                fmt_plain, fmt_seconds, standalone=standalone, note=note)
    return {k: v for k, v in out.items() if v}


CSS = """
:root{color-scheme:light dark;--bg:#fff;--fg:#24292f;--muted:#57606a;--line:#d0d7de;--ok:#1a7f37;--bad:#cf222e;--skip:#9a6700;--card:#f6f8fa}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--muted:#8b949e;--line:#30363d;--ok:#3fb950;--bad:#f85149;--skip:#d29922;--card:#161b22}}
body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;max-width:980px;margin:0 auto;padding:16px}
h1{font-size:1.5rem}h2{font-size:1.15rem;margin-top:2rem;border-bottom:1px solid var(--line);padding-bottom:.2rem}
table{border-collapse:collapse;width:100%;font-size:.9rem}th,td{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}
th{color:var(--muted);font-weight:600}.num{text-align:right;font-variant-numeric:tabular-nums}
.ok{color:var(--ok);font-weight:600}.FAIL{color:var(--bad);font-weight:600}.skip{color:var(--skip);font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 12px;margin:8px 0}
svg{width:100%;height:auto;margin:8px 0}.muted{color:var(--muted)}
"""


def machine_table(runs):
    keys = [("cpu", "CPU"), ("cpus", "logical CPUs"), ("memory_mb", "memory MB"), ("os", "system"), ("kernel", "kernel"),
            ("arch", "arch"), ("compiler", "compiler"), ("version", "ibnetlens"), ("commit", "commit"), ("simd", "SIMD hex parser"),
            ("gpu", "GPU"), ("python", "python"), ("date", "date")]
    rows = ["<tr><th></th>%s</tr>" % "".join("<th>%s</th>" % html.escape(r["label"]) for r in runs)]
    for k, label in keys:
        vals = [r["machine"].get(k, "") for r in runs]
        if not any(str(v) != "" for v in vals):
            continue
        rows.append("<tr><th>%s</th>%s</tr>" % (label, "".join("<td>%s</td>" % html.escape(str(v)) for v in vals)))
    return "<table>%s</table>" % "".join(rows)


def tests_section(run):
    t = run["tests"]
    if not t:
        return ""
    s = t["summary"]
    parts = ['<h2>Tests - %s</h2>' % html.escape(run["label"])]
    parts.append('<div class="card"><span class="ok">%d passed</span> · <span class="FAIL">%d failed</span> · '
                 '<span class="skip">%d skipped</span> <span class="muted">in %.1f s</span></div>' % (
                     s["ok"], s["fail"], s["skip"], t.get("seconds", 0)))
    suites = {}
    for r in t["results"]:
        suites.setdefault(r["suite"], []).append(r)
    for name, items in suites.items():
        bad = sum(1 for i in items if i["status"] == "FAIL")
        parts.append("<details%s><summary>%s <span class=\"muted\">(%d)</span></summary><table>" % (
            " open" if bad else "", html.escape(name), len(items)))
        for i in items:
            parts.append('<tr><td class="%s" style="width:4.5em">%s</td><td>%s</td></tr>' % (
                i["status"], i["status"], html.escape(i["name"])))
        parts.append("</table></details>")
    return "\n".join(parts)


def bench_tables(runs):
    parts = []
    for run in runs:
        b = run["bench"]
        if not b:
            continue
        parts.append("<h2>Measurements - %s</h2>" % html.escape(run["label"]))
        head = ["nodes", "size MB", "ibnetlens 1 thread", "ibnetlens all cores", "ibnetlens slurm", "ibnetlens check", "Python parser"]
        has_k = any(r.get("ibtopology_s") is not None for r in b["scaling"])
        if has_k:
            head.append("ibtopology")
        rows = ["<tr>%s</tr>" % "".join('<th class="num">%s</th>' % h for h in head)]
        for r in b["scaling"]:
            cells = [fmt_count(r["nodes"]), "%.1f" % r["size_mb"]]
            for k in ("ibnetlens_j1_s", "ibnetlens_auto_s", "ibnetlens_slurm_s", "ibnetlens_check_s", "python_s") + (("ibtopology_s",) if has_k else ()):
                cells.append("-" if r.get(k) is None else fmt_seconds(r[k]))
            rows.append("<tr>%s</tr>" % "".join('<td class="num">%s</td>' % c for c in cells))
        parts.append("<table>%s</table>" % "".join(rows))
        st = b.get("startup") or {}
        if st.get("ibnetlens_s"):
            parts.append('<p class="muted">Start-up on a tiny file: ibnetlens %s, Python parser %s.</p>' % (
                fmt_seconds(st["ibnetlens_s"]), fmt_seconds(st["python_s"]) if st.get("python_s") else "n/a"))
    return "\n".join(parts)


def page(runs):
    c = charts(runs)
    body = ["<h1>ibnetlens test and benchmark report</h1>", machine_table(runs)]
    for run in runs:
        body.append(tests_section(run))
    if c:
        body.append("<h2>Charts</h2>")
        body.extend(c.values())
    body.append(bench_tables(runs))
    return ("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
            "content=\"width=device-width,initial-scale=1\"><title>ibnetlens report</title><style>%s%s</style></head><body>%s</body></html>"
            % (CSS, ".bg{display:none}.txt{fill:var(--fg);font:12px system-ui,sans-serif}.title{fill:var(--fg);font:600 14px system-ui,sans-serif}"
               ".axis{stroke:var(--muted);fill:none}.grid{stroke:var(--line)}.minor{stroke:var(--line);opacity:.5}", "\n".join(body)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("-o", "--output")
    ap.add_argument("--svg-dir")
    a = ap.parse_args()
    runs = [load_run(d) for d in a.dirs]
    out = a.output or os.path.join(a.dirs[0], "report.html")
    with open(out, "w") as fh:
        fh.write(page(runs))
    print("report:", out)
    if a.svg_dir:
        os.makedirs(a.svg_dir, exist_ok=True)
        for name, svg in charts(runs, standalone=True).items():
            with open(os.path.join(a.svg_dir, name + ".svg"), "w") as fh:
                fh.write(svg)
        print("charts:", a.svg_dir)


if __name__ == "__main__":
    sys.exit(main())
