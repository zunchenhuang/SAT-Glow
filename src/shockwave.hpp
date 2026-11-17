// Conflict shockwaves.  When the solver learns a clause, a wave of light
// starts at the clause's variables and runs outwards along the edges of the
// variable graph, one hop at a time, while a ring expands on screen around
// them.  Longer backjumps make stronger waves.
#pragma once
#include <glad/glad.h>

#include <cstdint>
#include <vector>

#include "graph.hpp"

class Shockwaves {
public:
  void init (const Graph &g); // needs a current OpenGL context

  // Starts a wave from variables vars[0..k-1] (1-based), strength 0..1.
  void spawn (const int32_t *vars, int k, float strength, float now);

  // Per-node brightness boost of all running waves at time `now`.
  void update (float now, std::vector<float> &boost);

  // Draws the rings into the current framebuffer (w x h pixels).
  // `pos` holds x, y, z per node; `mvp` is the camera matrix.
  void draw_rings (const std::vector<float> &pos, const float *mvp, int w, int h, float now, float gain);

  void clear () { waves_.clear (); }
  int running () const { return (int) waves_.size (); }

  static constexpr float HOP_DELAY = 0.075f; // seconds per graph hop
  static constexpr int MAX_HOPS = 6;
  static constexpr float LIFE = 1.3f;        // seconds a wave lives

private:
  struct Wave {
    float t0, amp;
    std::vector<uint32_t> nodes; // reached nodes ...
    std::vector<uint8_t> hop;    // ... and at which hop
    size_t seeds = 0;            // the first `seeds` nodes are the clause
  };
  std::vector<uint32_t> off_, adj_;
  std::vector<uint32_t> stamp_;
  uint32_t epoch_ = 0;
  std::vector<Wave> waves_;
  std::vector<float> verts_;
  GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
};
