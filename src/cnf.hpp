// DIMACS CNF loader.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cnf {
  std::string name;            // file name without directory
  int nvars = 0;
  std::vector<int> lits;       // all literals, clause after clause
  std::vector<uint32_t> start; // clause i is lits[start[i] .. start[i+1])

  size_t nclauses () const { return start.empty () ? 0 : start.size () - 1; }
  const int *begin (size_t i) const { return lits.data () + start[i]; }
  const int *end (size_t i) const { return lits.data () + start[i + 1]; }
};

// Reads plain DIMACS, or .gz / .xz / .bz2 through the matching command line
// tool.  Returns false and fills `err` on failure.
bool parse_dimacs (const std::string &path, Cnf &out, std::string &err);
