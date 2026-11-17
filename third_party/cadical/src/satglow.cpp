// satglow: read access to two of CaDiCaL's own statistics, used to check
// that the visualization hooks do not change the search.
#include "internal.hpp"

namespace CaDiCaL {

void Solver::satglow_counts (int64_t &conflicts, int64_t &decisions) const {
  conflicts = internal->stats.conflicts;
  decisions = internal->stats.decisions;
}

} // namespace CaDiCaL
