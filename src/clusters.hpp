// Clustering for large formulas.
//
// The variable graph is split into communities with the Louvain method
// (Blondel et al., 2008).  Only the much smaller graph of communities is
// laid out by the force layout; inside its community every variable gets a
// fixed place on a spiral, in breadth-first order so that neighbours sit
// close together.  Zoomed out, a community reads as one patch of light;
// zoomed in, its variables are visible one by one.
#pragma once
#include <vector>

#include "graph.hpp"

struct Clustering {
  int k = 0;               // number of clusters
  std::vector<int> of;     // cluster of each node
  std::vector<int> size;   // members per cluster
  std::vector<float> off2; // per node: place inside its cluster, flat (x, y)
  std::vector<float> off3; // per node: place inside its cluster, ball (x, y, z)
  Graph cgraph;            // graph of clusters, for the layout
  double modularity = 0;
  int levels = 0;
  double seconds = 0;
};

void build_clustering (const Graph &g, Clustering &c);

// Turns cluster positions (3 per cluster) into node positions (3 per node).
// depth_mix goes from 0 (flat) to 1 (3D).  `spread` scales the clusters.
// The radius of every cluster is stored in `radius`.
void expand_positions (const Clustering &c, const std::vector<float> &cpos, float depth_mix, float spread,
                       std::vector<float> &pos, std::vector<float> &radius);
