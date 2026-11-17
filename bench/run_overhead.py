#!/usr/bin/env python3
"""Overhead and fidelity benchmark for the SATGlow solver hooks.

Runs every instance with every solver in several modes and appends one CSV
line per run (resumable: finished runs are skipped).

modes:
  base     satglow-check-base: solver built with all hooks compiled out
  idle     satglow-check --no-hooks: hooks compiled in, no context attached
  viz      satglow-check: hooks active, as when the visualizer runs at full speed
  trace    satglow-check --trace /dev/null: every event also serialized
  metrics  satglow-check --metrics: events also analysed online

Parallel runs (--jobs) are fine for screening which instances finish, but
time measurements should run one at a time (--jobs 1, the default).

Example (one run at a time for clean timings):
  ./run_overhead.py --instances instances/main_2023 --timeout 1800 --reps 1 \\
                    --out results/overhead.csv
"""
import argparse
import concurrent.futures
import csv
import os
import random
import subprocess
import sys
import threading
import time

HERE = os.path.dirname (os.path.abspath (__file__))
CNF_EXT = (".cnf", ".cnf.xz", ".cnf.gz", ".cnf.bz2")


def find_instances (paths):
    found = []
    for p in paths:
        if os.path.isdir (p):
            for root, _, files in os.walk (p):
                for f in files:
                    if f.endswith (CNF_EXT):
                        found.append (os.path.join (root, f))
        elif p.endswith (".txt"):
            with open (p) as fh:
                found += [l.strip () for l in fh if l.strip () and not l.startswith ("#")]
        else:
            found.append (p)
    return sorted (set (found))


def command (build, mode, solver, inst, timeout):
    if mode == "base":
        exe = os.path.join (build, "satglow-check-base")
        extra = []
    else:
        exe = os.path.join (build, "satglow-check")
        extra = {"idle": ["--no-hooks"], "viz": [], "trace": ["--trace", "/dev/null"],
                 "metrics": ["--metrics", "/dev/null"]}[mode]
    return [exe, "--csv", "--solver", solver, "--timeout", str (timeout)] + extra + [inst]


def main ():
    ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument ("--instances", nargs="+", required=True, help="directories, files, or .txt lists")
    ap.add_argument ("--build", default=os.path.join (HERE, "..", "build"))
    ap.add_argument ("--solvers", nargs="+", default=["cadical", "kissat", "minisat"])
    ap.add_argument ("--modes", nargs="+", default=["base", "idle", "viz", "trace"])
    ap.add_argument ("--timeout", type=float, default=600)
    ap.add_argument ("--reps", type=int, default=1)
    ap.add_argument ("--out", default=os.path.join (HERE, "results", "overhead.csv"))
    ap.add_argument ("--seed", type=int, default=1, help="shuffles the run order")
    ap.add_argument ("--jobs", type=int, default=1, help="parallel runs (screening only)")
    args = ap.parse_args ()

    insts = find_instances (args.instances)
    if not insts:
        sys.exit ("no instances found")
    os.makedirs (os.path.dirname (os.path.abspath (args.out)), exist_ok=True)
    header = subprocess.run ([os.path.join (args.build, "satglow-check"), "--csv-header"],
                             capture_output=True, text=True, check=True).stdout.strip ()
    fields = ["path", "rep", "wall"] + header.split (",")
    done = set ()
    if os.path.exists (args.out):
        with open (args.out) as fh:
            for row in csv.DictReader (fh):
                done.add ((row["path"], row["solver"], row["mode"] if row["build"] == "hooked" else "base", row["rep"]))
    else:
        with open (args.out, "w") as fh:
            fh.write (",".join (fields) + "\n")

    # Interleave modes per instance and shuffle, so slow drifts of the
    # machine do not favour one mode.
    runs = [(i, s, m, r) for i in insts for s in args.solvers for r in range (args.reps) for m in args.modes]
    random.Random (args.seed).shuffle (runs)
    todo = [x for x in runs if (x[0], x[1], x[2], str (x[3])) not in done]
    print (f"{len (insts)} instances, {len (runs)} runs, {len (todo)} still to do", flush=True)
    lock = threading.Lock ()
    counter = [0]

    def one (job):
        inst, solver, mode, rep = job
        cmd = command (args.build, mode, solver, inst, args.timeout)
        t0 = time.time ()
        try:
            p = subprocess.run (cmd, capture_output=True, text=True, timeout=args.timeout + 600)
            out, err = p.stdout.strip (), p.stderr.strip ()
        except subprocess.TimeoutExpired:
            out, err = "", "hard timeout"
        wall = time.time () - t0
        line = out.splitlines ()[-1] if out else ""
        with lock:
            counter[0] += 1
            k = counter[0]
            if not line or line.count (",") != header.count (","):
                print (f"[{k}/{len (todo)}] FAILED {mode} {solver} {os.path.basename (inst)}: {err[:200]}", flush=True)
                return
            with open (args.out, "a") as fh:
                fh.write (f"{inst},{rep},{wall:.3f},{line}\n")
            f = line.split (",")
            print (f"[{k}/{len (todo)}] {mode:7s} {solver:8s} {f[4]:7s} {float (f[5]):9.3f}s {os.path.basename (inst)}",
                   flush=True)

    if args.jobs <= 1:
        for job in todo:
            one (job)
    else:
        with concurrent.futures.ThreadPoolExecutor (max_workers=args.jobs) as pool:
            list (pool.map (one, todo))

if __name__ == "__main__":
    main ()
