// Variable interaction graph (VIG): one node per variable, an edge between
// two variables that occur together in a clause.
#pragma once
#include <cstdint>
#include <vector>

#include "cnf.hpp"

struct Graph {
  int n = 0;                   // node i is variable i + 1
  std::vector<uint32_t> edges; // two node indices per edge
  std::vector<float> weight;   // one weight per edge
  std::vector<float> mass;     // degree + 1, per node
  size_t nedges () const { return weight.size (); }
};

// Clauses up to `clique_limit` variables become cliques; longer ones become
// rings, which keeps the edge count linear in the formula size.
void build_vig (const Cnf &cnf, Graph &g, int clique_limit = 8);

// Neighbour lists of `g` in compressed form: the neighbours of node i are
// adj[off[i]] .. adj[off[i + 1] - 1].
void build_adjacency (const Graph &g, std::vector<uint32_t> &off, std::vector<uint32_t> &adj);
