#include "intro.hpp"

#include <algorithm>
#include <cmath>

#include "glutil.hpp"

namespace {

const char *FLAT_VS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in float aSize;
uniform vec2 uScale;
out vec4 vCol;
void main() {
  gl_Position = vec4(aPos * uScale + vec2(-1.0, 1.0), 0.0, 1.0);
  gl_PointSize = max(aSize, 1.0);
  vCol = aCol;
}
)";

const char *FLAT_FS = R"(#version 330 core
in vec4 vCol;
out vec4 oCol;
void main() { oCol = vCol; }
)";

// Round, soft point: uSoft = 1 gives a wide halo, 0 a crisp spark.
const char *SPRITE_FS = R"(#version 330 core
in vec4 vCol;
uniform float uSoft;
out vec4 oCol;
void main() {
  vec2 p = gl_PointCoord * 2.0 - 1.0;
  float d2 = dot(p, p);
  if (d2 > 1.0) discard;
  float d = sqrt(d2);
  float spark = smoothstep(0.55, 0.20, d) + 0.5 * exp(-d2 * 6.0) * (1.0 - d);
  float halo = exp(-d2 * 3.0) * (1.0 - d);
  oCol = vec4(vCol.rgb, vCol.a * mix(spark, halo, uSoft));
}
)";

float hash01 (uint32_t i) {
  i ^= i >> 16;
  i *= 0x7feb352dU;
  i ^= i >> 15;
  i *= 0x846ca68bU;
  i ^= i >> 16;
  return (float) (i & 0xffffff) / (float) 0x1000000;
}

float clamp01 (float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

const float FLIGHT = 0.95f; // seconds a triangle needs to fly home

} // namespace

float Intro::cover (float t) {
  if (t < FADE_START)
    return 1.0f;
  const float u = clamp01 (1.0f - (t - FADE_START) / (END - FADE_START));
  return u * u * (3.0f - 2.0f * u);
}

void Intro::init () {
  build_logo (mesh_);
  const size_t nt = mesh_.ntriangles ();
  shards_.resize (nt);
  for (size_t t = 0; t < nt; t++) {
    Shard &s = shards_[t];
    float x[3], y[3];
    s.cx = s.cy = 0;
    for (int k = 0; k < 3; k++) {
      x[k] = mesh_.vx[mesh_.tri[3 * t + (size_t) k]];
      y[k] = mesh_.vy[mesh_.tri[3 * t + (size_t) k]];
      s.cx += x[k] / 3.0f;
      s.cy += y[k] / 3.0f;
    }
    for (int k = 0; k < 3; k++) {
      s.px[k] = x[k] - s.cx;
      s.py[k] = y[k] - s.cy;
    }
    const uint32_t h = (uint32_t) t * 2654435761u;
    const float angle = 6.2831853f * hash01 (h + 1u);
    const float radius = 1.3f + 2.6f * hash01 (h + 2u);
    s.ox = radius * std::cos (angle) * 1.5f;
    s.oy = radius * std::sin (angle);
    s.spin = (hash01 (h + 3u) - 0.5f) * 9.0f;
    // Take off from left to right, with some disorder.
    s.delay = 0.45f + 1.05f * (s.cx / mesh_.width) + 0.45f * hash01 (h + 4u);
    s.phase = 6.2831853f * hash01 (h + 5u);
    s.r = mesh_.rgb[3 * t];
    s.g = mesh_.rgb[3 * t + 1];
    s.b = mesh_.rgb[3 * t + 2];
  }

  prog_flat_ = make_program (FLAT_VS, FLAT_FS);
  prog_sprite_ = make_program (FLAT_VS, SPRITE_FS);
  glGenVertexArrays (1, &vao_);
  glBindVertexArray (vao_);
  glGenBuffers (1, &vbo_);
  glBindBuffer (GL_ARRAY_BUFFER, vbo_);
  const GLsizei stride = (GLsizei) sizeof (Vertex);
  glEnableVertexAttribArray (0);
  glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, stride, (void *) 0);
  glEnableVertexAttribArray (1);
  glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, stride, (void *) (2 * sizeof (float)));
  glEnableVertexAttribArray (2);
  glVertexAttribPointer (2, 1, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof (float)));
  glBindVertexArray (0);
}

void Intro::draw (float t, int fbw, int fbh) {
  const float W = (float) fbw, H = (float) fbh;
  const float cov = cover (t);
  if (cov <= 0)
    return;

  // Logo placement: 64% of the width, a little above the middle.
  const float logo_w = std::min (0.64f * W, 0.40f * H * mesh_.width / mesh_.height);
  const float s = logo_w / mesh_.width; // pixels per logo unit
  const float x0 = 0.5f * (W - logo_w);
  const float base = 0.47f * H + 0.5f * mesh_.height * s; // baseline
  logo_left = x0;
  logo_right = x0 + logo_w;
  logo_bottom = base;

  const size_t nt = shards_.size ();
  verts_.clear ();
  verts_.reserve (6 + nt * 15);

  // Backdrop that hides the visualizer until the logo fades.
  const float br = 0.012f, bg = 0.014f, bb = 0.026f;
  const Vertex q[4] = {{0, 0, br, bg, bb, cov, 1}, {W, 0, br, bg, bb, cov, 1}, {W, H, br, bg, bb, cov, 1}, {0, H, br, bg, bb, cov, 1}};
  for (int k : {0, 1, 2, 0, 2, 3})
    verts_.push_back (q[k]);

  const size_t halo0 = verts_.size ();
  verts_.resize (halo0 + nt * 3);                 // soft halo sprites
  const size_t tri0 = verts_.size ();
  verts_.resize (tri0 + nt * 3);                  // filled triangles
  const size_t line0 = verts_.size ();
  verts_.resize (line0 + nt * 6);                 // outlines
  const size_t point0 = verts_.size ();
  verts_.resize (point0 + nt * 3);                // corner sparks

  const float appear = clamp01 (t / 0.5f);
  const float sweep_pos = (t - 2.95f) / 0.7f * 1.3f - 0.15f;

  for (size_t i = 0; i < nt; i++) {
    const Shard &sh = shards_[i];
    const float e = clamp01 ((t - sh.delay) / FLIGHT);
    const float k = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e); // ease out
    const float away = 1.0f - k;

    // Position: from a drifting spot in the cloud to its place in the logo.
    const float drift_x = 0.10f * std::sin (0.8f * t + sh.phase);
    const float drift_y = 0.10f * std::cos (0.6f * t + 1.7f * sh.phase);
    const float cx = sh.cx + (sh.ox + drift_x) * away;
    const float cy = sh.cy + (sh.oy + drift_y) * away;
    const float angle = (sh.spin + 0.5f * std::sin (1.1f * t + sh.phase)) * away;
    const float scale = 0.5f + 0.5f * k;
    const float ca = std::cos (angle) * scale, sa = std::sin (angle) * scale;

    // Colour: dim and grey while loose (an unassigned variable), a white
    // flash on landing (a decision), then its final amber or blue.
    float r = 0.17f + (sh.r - 0.17f) * k * k;
    float g = 0.21f + (sh.g - 0.21f) * k * k;
    float b = 0.36f + (sh.b - 0.36f) * k * k;
    const float since = (t - (sh.delay + FLIGHT)) / 0.16f;
    float flash = since > -2.0f ? std::exp (-since * since) : 0.0f;
    // Afterwards one bright wave runs over the finished logo.
    const float du = (sh.cx / mesh_.width - sweep_pos) / 0.075f;
    flash = std::max (flash, 0.7f * std::exp (-du * du));
    r += (1.0f - r) * 0.8f * flash;
    g += (1.0f - g) * 0.8f * flash;
    b += (1.0f - b) * 0.8f * flash;

    const float alpha = appear * cov;
    const float fill_a = alpha * (0.45f + 0.55f * k);
    const float lr = std::min (1.0f, r + 0.28f), lg = std::min (1.0f, g + 0.28f), lb = std::min (1.0f, b + 0.28f);
    Vertex corner[3];
    for (int c = 0; c < 3; c++) {
      const float x = cx + sh.px[c] * ca - sh.py[c] * sa;
      const float y = cy + sh.px[c] * sa + sh.py[c] * ca;
      corner[c] = {x0 + x * s, base - y * s, r, g, b, fill_a, 1.0f};
    }
    for (int c = 0; c < 3; c++) {
      verts_[tri0 + i * 3 + (size_t) c] = corner[c];
      Vertex halo = corner[c];
      halo.a = alpha * (0.035f + 0.085f * k + 0.10f * flash);
      halo.size = (0.34f + 0.20f * flash) * s;
      verts_[halo0 + i * 3 + (size_t) c] = halo;
      Vertex a = corner[c], d = corner[(c + 1) % 3];
      a.r = d.r = lr, a.g = d.g = lg, a.b = d.b = lb;
      a.a = d.a = alpha * 0.55f;
      verts_[line0 + i * 6 + (size_t) c * 2] = a;
      verts_[line0 + i * 6 + (size_t) c * 2 + 1] = d;
      Vertex p = corner[c];
      p.r = 0.6f + 0.4f * r, p.g = 0.6f + 0.4f * g, p.b = 0.6f + 0.4f * b;
      p.a = alpha * (0.35f + 0.45f * k + 0.2f * flash);
      p.size = std::max (2.0f, (0.050f + 0.05f * flash) * s);
      verts_[point0 + i * 3 + (size_t) c] = p;
    }
  }

  glViewport (0, 0, fbw, fbh);
  glDisable (GL_DEPTH_TEST);
  glDisable (GL_SCISSOR_TEST);
  glEnable (GL_BLEND);
  glEnable (GL_PROGRAM_POINT_SIZE);
  glBindVertexArray (vao_);
  glBindBuffer (GL_ARRAY_BUFFER, vbo_);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (verts_.size () * sizeof (Vertex)), verts_.data (), GL_STREAM_DRAW);

  glUseProgram (prog_flat_);
  glUniform2f (glGetUniformLocation (prog_flat_, "uScale"), 2.0f / W, -2.0f / H);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays (GL_TRIANGLES, 0, 6);

  glUseProgram (prog_sprite_);
  glUniform2f (glGetUniformLocation (prog_sprite_, "uScale"), 2.0f / W, -2.0f / H);
  glUniform1f (glGetUniformLocation (prog_sprite_, "uSoft"), 1.0f);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE); // light adds up
  glDrawArrays (GL_POINTS, (GLint) halo0, (GLsizei) (nt * 3));

  glUseProgram (prog_flat_);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays (GL_TRIANGLES, (GLint) tri0, (GLsizei) (nt * 3));
  glBlendFunc (GL_SRC_ALPHA, GL_ONE);
  glDrawArrays (GL_LINES, (GLint) line0, (GLsizei) (nt * 6));

  glUseProgram (prog_sprite_);
  glUniform1f (glGetUniformLocation (prog_sprite_, "uSoft"), 0.0f);
  glDrawArrays (GL_POINTS, (GLint) point0, (GLsizei) (nt * 3));

  glBindVertexArray (0);
  glUseProgram (0);
  glDisable (GL_BLEND);
}
