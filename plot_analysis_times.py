#!/usr/bin/env python3
"""
Plot per-target analysis wall-clock times as a horizontal bar chart.

Reads each analysis/<target>/<target>_analysis_time.txt (the `time` builtin
output, e.g. "real\t6m29.943s") and graphs the `real` time.

Usage:
    ./plot_analysis_times.py                 # all targets with a time file
    ./plot_analysis_times.py c-ares sqlite   # only the named targets
    ./plot_analysis_times.py --metric user   # graph user/sys instead of real
    ./plot_analysis_times.py -o out.png      # choose output path
"""
import argparse
import glob
import os
import re

import matplotlib
matplotlib.use("Agg")  # headless (WSL has no display by default)
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.abspath(__file__))
TIME_RE = re.compile(r"^(real|user|sys)\s+(\d+)m([\d.]+)s")


def parse_seconds(path):
    """Return {'real': s, 'user': s, 'sys': s} parsed from a time file."""
    vals = {}
    with open(path) as fh:
        for line in fh:
            m = TIME_RE.match(line.strip())
            if m:
                vals[m.group(1)] = int(m.group(2)) * 60 + float(m.group(3))
    return vals


def collect():
    data = {}
    for f in glob.glob(os.path.join(ROOT, "analysis", "*", "*_analysis_time.txt")):
        print("Analysis files found: ", f)
        target = os.path.basename(os.path.dirname(f))
        vals = parse_seconds(f)
        if vals:
            data[target] = vals
    return data


def fmt(seconds):
    m, s = divmod(int(round(seconds)), 60)
    return f"{m}m{s:02d}s" if m else f"{s}s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="*", help="targets to include (default: all)")
    ap.add_argument("--metric", choices=["real", "user", "sys"], default="real")
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "analysis_times.png"))
    args = ap.parse_args()

    data = collect()
    if args.targets:
        missing = [t for t in args.targets if t not in data]
        if missing:
            print(f"[WARN] no time data for: {', '.join(missing)}")
        data = {t: data[t] for t in args.targets if t in data}

    data = {t: v for t, v in data.items() if args.metric in v}
    if not data:
        print("No analysis-time data found.")
        return

    items = sorted(data.items(), key=lambda kv: kv[1][args.metric], reverse=True)
    names = [k for k, _ in items]
    secs = [v[args.metric] for _, v in items]

    fig, ax = plt.subplots(figsize=(10, max(4, 0.42 * len(names))))
    bars = ax.bar(names, secs, color="steelblue")
    ax.set_ylabel(f"Analysis {args.metric} time (s)")
    ax.set_title(f"Per-target analysis time ({args.metric})")
    ax.margins(x=0.1)
    for bar, s in zip(bars, secs):
        ax.text(bar.get_width() / 2 + bar.get_x(),
                bar.get_y() + bar.get_height() / 2,
                fmt(s), va="center", fontsize=8)
    fig.tight_layout()
    fig.savefig(args.output, dpi=150)
    print(f"Wrote {args.output}  ({len(names)} targets)")


if __name__ == "__main__":
    main()
