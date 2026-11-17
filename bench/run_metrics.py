#!/usr/bin/env python3
"""Search metrics for every instance and solver (case studies).

Runs satglow-check --metrics (and optionally --timeseries) and writes one
JSON per run plus a flat CSV table of the main metrics.

  ./run_metrics.py --instances instances/main_2023 --timeout 600 \\
                   --outdir results/metrics
"""
import argparse
import csv
import json
import os
import subprocess

from run_overhead import find_instances, HERE

COLUMNS = [
    ("variables", ["variables"]), ("clauses", ["clauses"]), ("communities", ["communities"]),
    ("modularity", ["modularity"]), ("result", ["result"]), ("solve_seconds", ["solve_seconds"]),
    ("conflicts", ["events", "conflicts"]), ("decisions", ["events", "decisions"]),
    ("restarts", ["events", "restarts"]), ("learned", ["learned_clauses_count"]),
    ("mean_size", ["learned_clauses", "mean_size"]), ("mean_glue", ["learned_clauses", "mean_glue"]),
    ("mean_span", ["learned_clauses", "mean_span"]), ("expected_span", ["learned_clauses", "expected_mean_span"]),
    ("locality", ["learned_clauses", "locality"]),
    ("single_community", ["learned_clauses", "single_community_fraction"]),
    ("expected_single", ["learned_clauses", "expected_single_community_fraction"]),
    ("focus_switch_rate", ["learned_clauses", "focus_switch_rate"]),
    ("backjump_mean", ["backjumps", "mean_distance"]),
    ("backjump_le1", ["backjumps", "distance_at_most_1_fraction"]),
    ("mean_level_at_conflict", ["search", "mean_level_at_conflict"]),
    ("trail_at_conflict", ["search", "mean_trail_fraction_at_conflict"]),
    ("assignment_gini", ["activity", "assignment_gini"]),
    ("learned_gini", ["activity", "learned_occurrence_gini"]),
    ("community_gini", ["activity", "community_learned_gini"]),
    ("top10_share", ["activity", "top10pct_communities_share"]),
    ("communities_touched", ["activity", "communities_touched_fraction"]),
]


def get (d, path):
    for k in path:
        if not isinstance (d, dict) or k not in d:
            return ""
        d = d[k]
    return d


def main ():
    ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument ("--instances", nargs="+", required=True)
    ap.add_argument ("--build", default=os.path.join (HERE, "..", "build"))
    ap.add_argument ("--solvers", nargs="+", default=["cadical", "kissat", "minisat"])
    ap.add_argument ("--timeout", type=float, default=600)
    ap.add_argument ("--timeseries", action="store_true", help="also write a CSV time series per run")
    ap.add_argument ("--interval", type=int, default=1000)
    ap.add_argument ("--outdir", default=os.path.join (HERE, "results", "metrics"))
    args = ap.parse_args ()

    insts = find_instances (args.instances)
    os.makedirs (args.outdir, exist_ok=True)
    rows = []
    for k, inst in enumerate (insts, 1):
        name = os.path.basename (inst)
        for solver in args.solvers:
            js = os.path.join (args.outdir, f"{name}.{solver}.json")
            if not os.path.exists (js):
                cmd = [os.path.join (args.build, "satglow-check"), "--csv", "--solver", solver,
                       "--timeout", str (args.timeout), "--metrics", js]
                if args.timeseries:
                    cmd += ["--timeseries", os.path.join (args.outdir, f"{name}.{solver}.csv"),
                            "--interval", str (args.interval)]
                p = subprocess.run (cmd + [inst], capture_output=True, text=True)
                if p.returncode not in (0, 1) or not os.path.exists (js):
                    print (f"[{k}/{len (insts)}] FAILED {solver} {name}: {p.stderr.strip ()[:200]}", flush=True)
                    continue
            with open (js) as fh:
                d = json.load (fh)
            d["learned_clauses_count"] = get (d, ["events", "learned"])
            rows.append ([name, solver] + [get (d, path) for _, path in COLUMNS])
            print (f"[{k}/{len (insts)}] {solver:8s} {name}  locality {get (d, ['learned_clauses', 'locality'])}",
                   flush=True)
    table = os.path.join (args.outdir, "metrics.csv")
    with open (table, "w", newline="") as fh:
        w = csv.writer (fh)
        w.writerow (["instance", "solver"] + [c for c, _ in COLUMNS])
        w.writerows (rows)
    print (f"wrote {table}")


if __name__ == "__main__":
    main ()
