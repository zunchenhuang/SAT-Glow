// Bloom: the graph is drawn into a floating-point framebuffer, bright parts
// are blurred over a chain of smaller and smaller copies ("dual filter"
// blur), and the result is added back on top.  A soft tone curve then maps
// the light, which can exceed 1 where many nodes overlap, to the screen.
#pragma once
#include <glad/glad.h>

#include <vector>

class Bloom {
public:
  void init ();

  // Redirects drawing into the HDR buffer (w x h pixels) and clears it.
  void begin (int w, int h, float r, float g, float b);

  // Blurs, adds the bloom, tone-maps and writes the picture into the
  // current viewport of the default framebuffer.
  void finish (int vx, int vy, int vw, int vh, bool enabled, float strength, float threshold);

private:
  struct Level {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
  };
  void resize (int w, int h);
  static void make_target (Level &l, int w, int h);
  static void free_target (Level &l);
  void quad ();

  Level scene_;
  std::vector<Level> mips_;
  GLuint prog_down_ = 0, prog_up_ = 0, prog_out_ = 0, vao_ = 0;
  int w_ = 0, h_ = 0;
};
