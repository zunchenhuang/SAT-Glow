// Force-directed layout of the variable interaction graph, running in its
// own thread so that the picture keeps settling while the solver works.
//
// The force model is ForceAtlas2 (Jacomy et al., 2014) with Barnes-Hut
// approximation for the repulsion.  It works in two or three dimensions and
// can be switched while running: going to 3D gives every node a little
// random depth and lets the forces inflate the graph, going back to 2D
// squashes the depth away again.
#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "graph.hpp"

class Layout {
public:
  ~Layout () { stop (); }
  // `init`, if given, holds starting positions (x, y, z per node) instead
  // of a random scatter.
  void start (const Graph *g, unsigned seed, bool three_d, const std::vector<float> *init = nullptr);
  void stop ();

  // Copies the latest node positions (x0, y0, z0, x1, y1, z1, ...) into
  // `xyz`.
  void snapshot (std::vector<float> &xyz);

  // Scatter the nodes again and start over.
  void reshuffle () { reshuffle_.store (true); }

  std::atomic<bool> running{true};  // false freezes the layout
  std::atomic<bool> three_d{false}; // lay out in 3D instead of 2D
  std::atomic<float> scaling{2.0f}; // repulsion strength
  std::atomic<float> gravity{1.0f}; // pull towards the centre
  std::atomic<uint64_t> iterations{0};
  std::atomic<float> ms_per_iteration{0.0f};

private:
  void run (unsigned seed);
  void iterate ();
  void scatter (unsigned seed);
  void inflate (unsigned seed);

  struct TNode {
    float mx, my, mz, mass; // sum of mass * position (then centre of mass)
    float cx, cy, cz, half; // cubic cell: centre and half side length
    int32_t child;          // index of first child, -1 for a leaf
    int32_t body;           // leaf: body index, -1 empty, -2 several bodies
  };
  void build_tree ();
  void repulse_range (int from, int to);

  const Graph *g_ = nullptr;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> reshuffle_{false};
  std::mutex mutex_;
  std::vector<float> published_;

  std::vector<float> x_, y_, z_, dx_, dy_, dz_, odx_, ody_, odz_;
  std::vector<TNode> tree_;
  double speed_ = 1.0, speed_efficiency_ = 1.0;
  int workers_ = 1;
  bool deep_ = false; // the forces currently act in three dimensions
};
