# SATLIB instances (pilot set)

51 classic real-world-derived instances from SATLIB (Hoos and Stützle,
https://www.cs.ubc.ca/~hoos/SATLIB/), as redistributed with the `ersatz`
library (https://github.com/ekmett/ersatz, `data/dimacs`):

| family | source | instances |
|---|---|---|
| bf | circuit fault analysis (bridge and stuck-at faults) | 12 |
| blocksworld | planning (SATPLAN) | 6 |
| bmc | bounded model checking, IBM hardware | 6 |
| logistics | planning (SATPLAN) | 4 |
| parity | learning a parity function (DIMACS) | 20 |
| phole | pigeonhole | 3 |

They are easy for modern solvers and serve as a quick pilot.  For the
paper, use the SAT Competition main track (`../../fetch_benchmarks.sh`).
