# SATGlow hook overhead and fidelity

51 instances, solvers: cadical, kissat, minisat, modes: base, idle, viz, trace; overhead over runs where base took at least 0.1 s.

## Solved instances

| solver | base | idle | viz | trace |
|---|---|---|---|---|
| cadical | 51/51 | 51/51 | 51/51 | 51/51 |
| kissat | 51/51 | 51/51 | 51/51 | 51/51 |
| minisat | 51/51 | 51/51 | 51/51 | 51/51 |

## Overhead (solve time relative to the hook-free build)

| solver | mode | instances | geometric mean | median | 90th pct | max |
|---|---|---|---|---|---|---|
| cadical | idle | 5 | 1.043 | 1.120 | 1.140 | 1.145 |
| cadical | viz | 5 | 1.008 | 1.054 | 1.077 | 1.078 |
| cadical | trace | 5 | 1.071 | 1.077 | 1.225 | 1.246 |
| kissat | idle | 5 | 1.133 | 1.039 | 1.472 | 1.602 |
| kissat | viz | 5 | 1.174 | 1.114 | 1.372 | 1.400 |
| kissat | trace | 5 | 1.152 | 1.186 | 1.200 | 1.204 |
| minisat | idle | 4 | 0.872 | 0.818 | 1.043 | 1.125 |
| minisat | viz | 4 | 0.959 | 0.958 | 1.135 | 1.190 |
| minisat | trace | 4 | 0.898 | 0.905 | 0.957 | 0.962 |

## Fidelity (identical search with and without hooks)

Runs that finished in both builds; the solver's own conflict and decision counters must match.

| solver | mode | compared | identical | different |
|---|---|---|---|---|
| cadical | idle | 51 | 51 | 0 |
| cadical | viz | 51 | 51 | 0 |
| cadical | trace | 51 | 51 | 0 |
| kissat | idle | 51 | 51 | 0 |
| kissat | viz | 51 | 51 | 0 |
| kissat | trace | 51 | 51 | 0 |
| minisat | idle | 51 | 51 | 0 |
| minisat | viz | 51 | 51 | 0 |
| minisat | trace | 51 | 51 | 0 |

Hook or model check failures: 0

## Search metrics (median over instances)

| solver | runs | communities | modularity | locality | single_community | expected_single | focus_switch_rate | backjump_mean | backjump_le1 | learned_gini | top10_share |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cadical | 47 | 12.000 | 0.734 | 1.378 | 0.221 | 0.050 | 0.574 | 1.006 | 0.995 | 0.932 | 0.358 |
| kissat | 28 | 12.000 | 0.729 | 1.550 | 0.105 | 0.043 | 0.620 | 1.230 | 0.849 | 0.941 | 0.328 |
| minisat | 43 | 11.000 | 0.730 | 1.457 | 0.155 | 0.016 | 0.500 | 1.200 | 0.844 | 0.968 | 0.395 |

locality = expected span of a random clause of the same size / observed span (> 1: learned clauses stay inside fewer communities than chance).

