/* satglow: read access to two of Kissat's own statistics, used to check
 * that the visualization hooks do not change the search. */
#include "internal.h"

void kissat_satglow_counts (kissat *solver, uint64_t *conflicts, uint64_t *decisions) {
  *conflicts = solver->statistics.conflicts;
  *decisions = solver->statistics.decisions;
}
