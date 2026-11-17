// Start-up animation: a cloud of loose triangles flies together and
// assembles the SATGlow wordmark, which then fades away to reveal the
// visualizer.  The assembled state is the logo (docs/logo.svg).
#pragma once
#include <vector>

#include "logo.hpp"

class Intro {
public:
  static constexpr float FADE_START = 3.9f; // seconds: logo complete, fade begins
  static constexpr float END = 4.7f;        // seconds: intro is over

  void init (); // needs a current OpenGL context

  // How much of the screen the intro still covers at time t: 1 .. 0.
  static float cover (float t);

  // Draws the animation at time t over the whole framebuffer.
  void draw (float t, int fbw, int fbh);

  const LogoMesh &mesh () const { return mesh_; }

  // Filled in by draw(): where the logo sits, in framebuffer pixels.
  float logo_left = 0, logo_right = 0, logo_bottom = 0;

private:
  struct Shard { // one triangle of the logo and how it flies in
    float cx, cy;       // final centroid (logo units)
    float px[3], py[3]; // corners relative to the centroid
    float ox, oy;       // where it starts, relative to the final place
    float spin;         // rotation at the start (radians)
    float delay;        // when it takes off (seconds)
    float phase;        // for the idle drift
    float r, g, b;      // final colour
  };
  struct Vertex {
    float x, y, r, g, b, a, size;
  };
  LogoMesh mesh_;
  std::vector<Shard> shards_;
  std::vector<Vertex> verts_;
  unsigned prog_flat_ = 0, prog_sprite_ = 0, vao_ = 0, vbo_ = 0;
};
