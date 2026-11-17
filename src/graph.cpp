#include "graph.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

void build_vig (const Cnf &cnf, Graph &g, int clique_limit) {
  g = Graph ();
  g.n = cnf.nvars;

  std::vector<std::pair<uint64_t, float>> pairs;
  pairs.reserve (cnf.lits.size () * 2);
  std::vector<uint32_t> vars;

  auto add = [&pairs] (uint32_t a, uint32_t b, float w) {
    if (a > b)
      std::swap (a, b);
    pairs.push_back ({((uint64_t) a << 32) | b, w});
  };

  for (size_t i = 0; i < cnf.nclauses (); i++) {
    vars.clear ();
    for (const int *p = cnf.begin (i); p != cnf.end (i); p++)
      vars.push_back ((uint32_t) (abs (*p) - 1));
    std::sort (vars.begin (), vars.end ());
    vars.erase (std::unique (vars.begin (), vars.end ()), vars.end ());
    const size_t k = vars.size ();
    if (k < 2)
      continue;
    if ((int) k <= clique_limit) {
      // Every variable is pulled with total strength 1 by each clause.
      const float w = 1.0f / (float) (k - 1);
      for (size_t a = 0; a < k; a++)
        for (size_t b = a + 1; b < k; b++)
          add (vars[a], vars[b], w);
    } else {
      for (size_t a = 0; a < k; a++)
        add (vars[a], vars[(a + 1) % k], 0.5f);
    }
  }

  std::sort (pairs.begin (), pairs.end (),
             [] (const std::pair<uint64_t, float> &x,
                 const std::pair<uint64_t, float> &y) {
               return x.first < y.first;
             });

  g.mass.assign ((size_t) g.n, 1.0f);
  for (size_t i = 0; i < pairs.size ();) {
    size_t j = i;
    float w = 0;
    while (j < pairs.size () && pairs[j].first == pairs[i].first)
      w += pairs[j++].second;
    uint32_t a = (uint32_t) (pairs[i].first >> 32);
    uint32_t b = (uint32_t) (pairs[i].first & 0xffffffffu);
    g.edges.push_back (a);
    g.edges.push_back (b);
    g.weight.push_back (w);
    g.mass[a] += 1.0f;
    g.mass[b] += 1.0f;
    i = j;
  }
}

void build_adjacency (const Graph &g, std::vector<uint32_t> &off, std::vector<uint32_t> &adj) {
  off.assign ((size_t) g.n + 1, 0);
  for (size_t k = 0; k < g.nedges (); k++) {
    off[g.edges[2 * k] + 1]++;
    off[g.edges[2 * k + 1] + 1]++;
  }
  for (int i = 0; i < g.n; i++)
    off[(size_t) i + 1] += off[(size_t) i];
  adj.resize (off.back ());
  std::vector<uint32_t> fill (off.begin (), off.end () - 1);
  for (size_t k = 0; k < g.nedges (); k++) {
    const uint32_t a = g.edges[2 * k], b = g.edges[2 * k + 1];
    adj[fill[a]++] = b;
    adj[fill[b]++] = a;
  }
}
