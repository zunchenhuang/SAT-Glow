# Benchmarks: overhead, fidelity and search metrics

These scripts produce the evaluation for a SATGlow paper. They answer three
questions:

1. **Fidelity.** Do the hooks change what the solver does? For every run
   that finishes, compare the solver's *own* conflict and decision counters
   between a build with all hooks compiled out and each hooked mode. They
   must be identical.
2. **Overhead.** How much slower is a solver with the hooks attached?
3. **Metrics.** What does the search look like relative to the structure of
   the formula? The headline metric is learned-clause **locality**: how
   many communities of the variable graph a learned clause spans, compared
   with a random clause of the same size.

## Programs (built in `../build`)

| program | what it is |
|---|---|
| `satglow-check-base` | the three solvers with every hook compiled out (`-DSG_DISABLE_HOOKS`): the baseline |
| `satglow-check --no-hooks` | hooks compiled in, but no context attached (mode `idle`) |
| `satglow-check` | hooks active, as when the visualizer runs at full speed (mode `viz`) |
| `satglow-check --trace FILE` | additionally writes every event to a trace (mode `trace`) |
| `satglow-check --metrics FILE.json` | additionally analyses the events while solving |
| `satglow-metrics TRACE CNF --metrics FILE.json` | the same analysis from a recorded trace |
| `satglow --replay TRACE CNF` | shows a recorded run in the visualizer |

All of them take `--solver cadical|kissat|minisat` and `--timeout S`;
`satglow-check --csv` prints one machine-readable line.

## Protocol for the paper (SAT Competition 2023 main track)

Prepare the machine: nothing else running, laptop on mains power, CPU
governor on `performance` (`sudo cpupower frequency-set -g performance`).

```bash
cd bench

# 1. Instances: 400 files from the Global Benchmark Database (GBD).
#    If the database cannot be reached, the same set is archived on Zenodo,
#    doi:10.5281/zenodo.11426992; unpack it into instances/main_2023/.
./fetch_benchmarks.sh main_2023

# 2. Screening: which instances does the hook-free build solve, and how
#    fast?  Parallel is fine here (about half your physical cores).
./run_overhead.py --instances instances/main_2023 --modes base \
    --timeout 600 --jobs 3 --out results/screen.csv

# 3. Selection: instances all three solvers solve in 1..600 s.  Use --any
#    to keep instances solved by at least one solver.
./select_solved.py results/screen.csv --min 1 --max 600 --out results/selected.txt

# 4. Timing: every mode, one run at a time, in shuffled order.
#    Resumable: just rerun the same command after an interruption.
./run_overhead.py --instances results/selected.txt \
    --modes base idle viz trace --timeout 1200 --reps 1 --out results/overhead.csv

# 5. Search metrics on the same instances.
./run_metrics.py --instances results/selected.txt --timeout 1200 \
    --timeseries --outdir results/metrics

# 6. Tables and plot.
./summarize.py results/overhead.csv --metrics results/metrics/metrics.csv \
    --min-time 1 --out results/summary
```

`summarize.py` writes `summary.md` (readable report), `summary-tables.tex`
(LaTeX tables) and `summary-overhead.png` (scatter plot of hook-free against
hooked solve times; needs matplotlib).

### Machine time

Screening costs at most 400 × 3 × 600 s and much less in practice, because
many instances finish early; with `--jobs 3` plan for about a day. The
timing stage depends on how many instances are selected: roughly the sum of
their base times × 4 modes. Run a smaller first pass with `--max 120` if you
want numbers sooner.

### Why two stages

Overhead can only be measured on runs that finish in both builds, and
instances that time out cost the full time limit in every mode. Screening
once with the cheapest build avoids paying that four times over.

### Notes

- `trace` mode writes to `/dev/null`, so it measures recording the events,
  not disk speed. Writing `.zst` traces needs `zstd`
  (`sudo apt install zstd`) and costs a little more.
- Traces are 8 bytes per event before compression. Hard instances produce
  hundreds of millions of events, so record traces for selected runs, not
  for the whole benchmark set.
- The community structure is computed once per instance, before solving,
  and is not part of the measured solve time. JSON field
  `structure_seconds` records it.

## Metrics (JSON fields)

| field | meaning |
|---|---|
| `communities`, `modularity` | Louvain communities of the variable graph and their modularity Q |
| `learned_clauses.mean_span` | average number of communities a learned clause touches |
| `learned_clauses.expected_mean_span` | the same for random clauses of the same sizes (exact, hypergeometric) |
| `learned_clauses.locality` | expected / observed span; above 1 means more local than chance |
| `learned_clauses.single_community_fraction` | learned clauses that lie inside one community (and its expected value) |
| `learned_clauses.focus_switch_rate` | how often the community most of a learned clause lies in changes from one clause to the next |
| `backjumps.mean_distance`, `distance_at_most_1_fraction`, `histogram` | backjump lengths in decision levels |
| `search.mean_level_at_conflict`, `mean_trail_fraction_at_conflict` | how deep and how full the trail is when conflicts happen |
| `activity.*_gini` | concentration of assignments / learned-clause occurrences over variables and communities (0 even, 1 concentrated) |
| `activity.top10pct_communities_share` | share of learned-clause occurrences in the busiest 10% of communities |

`--timeseries FILE.csv` writes the same quantities every `--interval`
conflicts, so phases of the search can be plotted.

## Pilot results (SATLIB, 51 instances)

`instances/satlib/` holds 51 classic SATLIB instances (circuit fault
analysis, IBM bounded model checking, planning, parity, pigeonhole); see
its README. They are easy for modern solvers (the longest hook-free run
takes 0.35 s), so they test the pipeline and fidelity, not overhead. On
this set, all 459 hooked runs (3 solvers × 3 hooked modes × 51 instances,
3 repetitions each) reproduced the hook-free build's conflict and decision
counts exactly. The overhead table from this pilot rests on 4 to 5
instances per solver lasting 0.1 to 0.35 s and is not meaningful.
