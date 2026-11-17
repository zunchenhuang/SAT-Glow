#!/usr/bin/env python3
"""Summarizes run_overhead.py (and run_metrics.py) results.

  ./summarize.py results/overhead.csv [--metrics results/metrics/metrics.csv]
                 [--min-time 1.0] [--out results/summary]

Writes <out>.md (readable report), <out>-tables.tex (LaTeX tables for the
paper) and, if matplotlib is installed, <out>-overhead.png.

Overhead is the ratio of solve times, mode over base, for instances that both
runs solved and where the base run took at least --min-time seconds (shorter
runs are dominated by timer noise).  Fidelity compares the solver's OWN
conflict and decision counters between the hook-free build and each hooked
mode: identical counters on a finished run mean the hooks did not change the
search.
"""
import argparse
import collections
import csv
import math
import os
import statistics


def gmean (xs):
    return math.exp (sum (math.log (x) for x in xs) / len (xs)) if xs else float ("nan")


def pct (xs, p):
    if not xs:
        return float ("nan")
    xs = sorted (xs)
    k = (len (xs) - 1) * p
    lo, hi = math.floor (k), math.ceil (k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def main ():
    ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument ("overhead")
    ap.add_argument ("--metrics")
    ap.add_argument ("--min-time", type=float, default=1.0)
    ap.add_argument ("--out", default=None)
    args = ap.parse_args ()
    out = args.out or os.path.join (os.path.dirname (os.path.abspath (args.overhead)), "summary")

    # (instance, solver, mode) -> list of runs
    runs = collections.defaultdict (list)
    for r in csv.DictReader (open (args.overhead)):
        mode = "base" if r["build"] == "base" else r["mode"]
        runs[(r["path"], r["solver"], mode)].append (r)
    solvers = sorted ({k[1] for k in runs})
    modes = [m for m in ["base", "idle", "viz", "trace", "metrics"] if any (k[2] == m for k in runs)]
    insts = sorted ({k[0] for k in runs})

    def median_time (rs):
        return statistics.median (float (r["seconds"]) for r in rs)

    md = []
    tex = []
    md.append (f"# SATGlow hook overhead and fidelity\n")
    md.append (f"{len (insts)} instances, solvers: {', '.join (solvers)}, modes: {', '.join (modes)}; "
               f"overhead over runs where base took at least {args.min_time:g} s.\n")

    # --- solved counts
    md.append ("## Solved instances\n")
    md.append ("| solver | " + " | ".join (modes) + " |")
    md.append ("|---|" + "---|" * len (modes))
    for s in solvers:
        cells = []
        for m in modes:
            solved = sum (1 for i in insts if runs.get ((i, s, m)) and runs[(i, s, m)][0]["result"] != "UNKNOWN")
            total = sum (1 for i in insts if runs.get ((i, s, m)))
            cells.append (f"{solved}/{total}")
        md.append (f"| {s} | " + " | ".join (cells) + " |")
    md.append ("")

    # --- overhead
    md.append ("## Overhead (solve time relative to the hook-free build)\n")
    md.append ("| solver | mode | instances | geometric mean | median | 90th pct | max |")
    md.append ("|---|---|---|---|---|---|---|")
    tex.append ("% overhead: geometric mean of time(mode)/time(base)")
    tex.append ("\\begin{tabular}{l" + "r" * (len (modes) - 1) + "}")
    tex.append ("\\toprule Solver & " + " & ".join (m for m in modes if m != "base") + " \\\\ \\midrule")
    ratios_all = {}
    for s in solvers:
        texcells = []
        for m in modes:
            if m == "base":
                continue
            ratios = []
            for i in insts:
                b, x = runs.get ((i, s, "base")), runs.get ((i, s, m))
                if not b or not x:
                    continue
                if b[0]["result"] == "UNKNOWN" or x[0]["result"] == "UNKNOWN":
                    continue
                tb, tx = median_time (b), median_time (x)
                if tb >= args.min_time and tb > 0 and tx > 0:
                    ratios.append (tx / tb)
            ratios_all[(s, m)] = ratios
            if ratios:
                g = gmean (ratios)
                md.append (f"| {s} | {m} | {len (ratios)} | {g:.3f} | {statistics.median (ratios):.3f} | "
                           f"{pct (ratios, 0.9):.3f} | {max (ratios):.3f} |")
                texcells.append (f"{(g - 1) * 100:+.1f}\\%")
            else:
                md.append (f"| {s} | {m} | 0 | – | – | – | – |")
                texcells.append ("--")
        tex.append (f"{s} & " + " & ".join (texcells) + " \\\\")
    tex.append ("\\bottomrule\\end{tabular}\n")
    md.append ("")

    # --- fidelity
    md.append ("## Fidelity (identical search with and without hooks)\n")
    md.append ("Runs that finished in both builds; the solver's own conflict and decision counters must match.\n")
    md.append ("| solver | mode | compared | identical | different |")
    md.append ("|---|---|---|---|---|")
    mismatches = []
    for s in solvers:
        for m in modes:
            if m == "base":
                continue
            comp = same = 0
            for i in insts:
                b, x = runs.get ((i, s, "base")), runs.get ((i, s, m))
                if not b or not x or b[0]["result"] == "UNKNOWN" or x[0]["result"] == "UNKNOWN":
                    continue
                comp += 1
                key = lambda r: (r["result"], r["own_conflicts"], r["own_decisions"])
                if all (key (r) == key (b[0]) for r in b + x):
                    same += 1
                else:
                    mismatches.append ((s, m, os.path.basename (i), key (b[0]), key (x[0])))
            md.append (f"| {s} | {m} | {comp} | {same} | {comp - same} |")
    if mismatches:
        md.append ("\nDifferences:\n")
        for s, m, i, kb, kx in mismatches[:50]:
            md.append (f"- {s} {m} {i}: base {kb}, {m} {kx}")
    failures = [r for rs in runs.values () for r in rs if r["hook_check"] != "ok" or r["model_check"] != "ok"]
    md.append (f"\nHook or model check failures: {len (failures)}")
    for r in failures[:20]:
        md.append (f"- {r['solver']} {r['mode']} {os.path.basename (r['path'])}: hook {r['hook_check']}, "
                   f"model {r['model_check']}")
    md.append ("")

    # --- metrics
    if args.metrics and os.path.exists (args.metrics):
        rows = list (csv.DictReader (open (args.metrics)))
        md.append ("## Search metrics (median over instances)\n")
        cols = ["communities", "modularity", "locality", "single_community", "expected_single", "focus_switch_rate",
                "backjump_mean", "backjump_le1", "learned_gini", "top10_share"]
        md.append ("| solver | runs | " + " | ".join (cols) + " |")
        md.append ("|---|---|" + "---|" * len (cols))
        for s in solvers:
            rs = [r for r in rows if r["solver"] == s and r["locality"] not in ("", "0", "0.0000")]
            cells = []
            for c in cols:
                vals = [float (r[c]) for r in rs if r[c] not in ("",)]
                cells.append (f"{statistics.median (vals):.3f}" if vals else "–")
            md.append (f"| {s} | {len (rs)} | " + " | ".join (cells) + " |")
        md.append ("\nlocality = expected span of a random clause of the same size / observed span "
                   "(> 1: learned clauses stay inside fewer communities than chance).\n")

    with open (out + ".md", "w") as fh:
        fh.write ("\n".join (md) + "\n")
    with open (out + "-tables.tex", "w") as fh:
        fh.write ("\n".join (tex) + "\n")
    print ("\n".join (md))
    print (f"\nwrote {out}.md and {out}-tables.tex")

    try:
        import matplotlib
        matplotlib.use ("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, axes = plt.subplots (1, len (solvers), figsize=(4.2 * len (solvers), 4), squeeze=False)
    for ax, s in zip (axes[0], solvers):
        for m, mark in [("viz", "o"), ("trace", "x"), ("idle", ".")]:
            xs, ys = [], []
            for i in insts:
                b, x = runs.get ((i, s, "base")), runs.get ((i, s, m))
                if b and x and b[0]["result"] != "UNKNOWN" and x[0]["result"] != "UNKNOWN":
                    xs.append (max (1e-3, median_time (b)))
                    ys.append (max (1e-3, median_time (x)))
            if xs:
                ax.scatter (xs, ys, s=12, marker=mark, label=m, alpha=0.7)
        lim = [1e-3, max ([1.0] + [float (r["seconds"]) for rs in runs.values () for r in rs]) * 1.5]
        ax.plot (lim, lim, "k-", lw=0.6)
        ax.plot (lim, [2 * v for v in lim], "k:", lw=0.6)
        ax.set_xscale ("log")
        ax.set_yscale ("log")
        ax.set_xlim (lim)
        ax.set_ylim (lim)
        ax.set_title (s)
        ax.set_xlabel ("hook-free solve time (s)")
        ax.set_ylabel ("with hooks (s)")
        ax.legend (fontsize=8)
    fig.tight_layout ()
    fig.savefig (out + "-overhead.png", dpi=150)
    print (f"wrote {out}-overhead.png")


if __name__ == "__main__":
    main ()
