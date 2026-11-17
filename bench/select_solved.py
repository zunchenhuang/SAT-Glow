#!/usr/bin/env python3
"""Picks the instances for the timing stage from a screening run.

  ./select_solved.py results/screen.csv --min 1 --max 600 --out results/selected.txt

Keeps instances that the hook-free build of EVERY listed solver solved, in
between --min and --max seconds (too short is timer noise, too long costs
too much machine time), and writes them one per line.
"""
import argparse
import csv
import collections

ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument ("screen")
ap.add_argument ("--min", type=float, default=1.0)
ap.add_argument ("--max", type=float, default=600.0)
ap.add_argument ("--solvers", nargs="+", default=None)
ap.add_argument ("--any", action="store_true", help="keep instances that ANY solver solved in the window")
ap.add_argument ("--out", required=True)
args = ap.parse_args ()

ok = collections.defaultdict (set)
seen = set ()
for r in csv.DictReader (open (args.screen)):
    if r["build"] != "base":
        continue
    seen.add (r["solver"])
    if r["result"] != "UNKNOWN" and args.min <= float (r["seconds"]) <= args.max:
        ok[r["path"]].add (r["solver"])
solvers = set (args.solvers or seen)
keep = sorted (p for p, s in ok.items () if (s & solvers if args.any else solvers <= s))
with open (args.out, "w") as fh:
    fh.write ("\n".join (keep) + "\n")
print (f"{len (keep)} instances written to {args.out}")
