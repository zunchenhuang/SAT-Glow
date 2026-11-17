#include "shockwave.hpp"

#include <algorithm>
#include <cmath>

#include "glutil.hpp"

namespace {

const char *RING_VS = R"(#version 330 core
layout(location = 0) in vec2 aPos;    // pixels
layout(location = 1) in vec4 aRing;   // centre x, y (pixels), radius, width
layout(location = 2) in vec4 aCol;    // colour and strength
uniform vec2 uSize;
out vec2 vPix;
flat out vec4 vRing;
flat out vec4 vCol;
void main() {
  gl_Position = vec4(aPos / uSize * 2.0 - 1.0, 0.0, 1.0);
  vPix = aPos;
  vRing = aRing;
  vCol = aCol;
}
)";

const char *RING_FS = R"(#version 330 core
in vec2 vPix;
flat in vec4 vRing;
flat in vec4 vCol;
out vec4 oCol;
void main() {
  float d = length(vPix - vRing.xy);
  float x = (d - vRing.z) / vRing.w;
  float ring = exp(-x * x * 2.5);
  // a hot thin core on a softer, wider band
  float core = exp(-x * x * 18.0);
  vec3 c = vCol.rgb * ring + vec3(1.0, 0.9, 0.8) * core * 0.6;
  oCol = vec4(c * vCol.a, 1.0);
}
)";

} // namespace

void Shockwaves::init (const Graph &g) {
  build_adjacency (g, off_, adj_);
  stamp_.assign ((size_t) g.n, 0);
  epoch_ = 0;
  waves_.clear ();
  if (!prog_) {
    prog_ = make_program (RING_VS, RING_FS);
    glGenVertexArrays (1, &vao_);
    glGenBuffers (1, &vbo_);
    glBindVertexArray (vao_);
    glBindBuffer (GL_ARRAY_BUFFER, vbo_);
    const GLsizei stride = 10 * sizeof (float);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, stride, (void *) 0);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, stride, (void *) (2 * sizeof (float)));
    glEnableVertexAttribArray (2);
    glVertexAttribPointer (2, 4, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof (float)));
    glBindVertexArray (0);
  }
}

void Shockwaves::spawn (const int32_t *vars, int k, float strength, float now) {
  const uint32_t n = (uint32_t) stamp_.size ();
  if (!n || k <= 0)
    return;
  if (waves_.size () >= 12) // keep the number of running waves bounded
    waves_.erase (waves_.begin ());
  if (++epoch_ == 0) {
    std::fill (stamp_.begin (), stamp_.end (), 0u);
    epoch_ = 1;
  }
  Wave w;
  w.t0 = now;
  w.amp = std::min (1.0f, std::max (0.15f, strength));
  for (int i = 0; i < k; i++) {
    const int32_t v = vars[i];
    if (v < 1 || (uint32_t) v > n)
      continue;
    const uint32_t node = (uint32_t) v - 1;
    if (stamp_[node] == epoch_)
      continue;
    stamp_[node] = epoch_;
    w.nodes.push_back (node);
    w.hop.push_back (0);
  }
  w.seeds = w.nodes.size ();
  if (!w.seeds)
    return;
  // Breadth-first search outwards, capped so that dense graphs stay cheap.
  const size_t cap = 6000;
  size_t head = 0;
  while (head < w.nodes.size () && w.nodes.size () < cap) {
    const uint32_t u = w.nodes[head];
    const uint8_t h = w.hop[head++];
    if (h >= MAX_HOPS)
      continue;
    for (uint32_t e = off_[u]; e < off_[u + 1] && w.nodes.size () < cap; e++) {
      const uint32_t x = adj_[e];
      if (stamp_[x] == epoch_)
        continue;
      stamp_[x] = epoch_;
      w.nodes.push_back (x);
      w.hop.push_back ((uint8_t) (h + 1));
    }
  }
  waves_.push_back (std::move (w));
}

void Shockwaves::update (float now, std::vector<float> &boost) {
  boost.assign (stamp_.size (), 0.0f);
  waves_.erase (std::remove_if (waves_.begin (), waves_.end (),
                                [now] (const Wave &w) { return now - w.t0 > LIFE; }),
                waves_.end ());
  const float width = 0.085f;
  for (const Wave &w : waves_) {
    const float age = now - w.t0;
    for (size_t i = 0; i < w.nodes.size (); i++) {
      const float x = (age - HOP_DELAY * (float) w.hop[i]) / width;
      if (x < -3.0f || x > 6.0f)
        continue;
      // Sharp rise when the wave arrives, slower fade behind it.
      const float shape = x < 0 ? std::exp (-x * x) : std::exp (-x * 0.6f);
      const float b = w.amp * shape / (1.0f + 0.30f * (float) w.hop[i]);
      float &dst = boost[w.nodes[i]];
      dst = std::max (dst, b);
    }
  }
}

void Shockwaves::draw_rings (const std::vector<float> &pos, const float *m, int w, int h, float now, float gain) {
  if (waves_.empty () || gain <= 0)
    return;
  verts_.clear ();
  const float scr = (float) std::min (w, h);
  for (const Wave &wv : waves_) {
    // Ring centre: the middle of the clause's variables on screen.
    float cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < wv.seeds; i++) {
      const size_t node = wv.nodes[i];
      cx += pos[3 * node];
      cy += pos[3 * node + 1];
      cz += pos[3 * node + 2];
    }
    cx /= (float) wv.seeds, cy /= (float) wv.seeds, cz /= (float) wv.seeds;
    const float cw = m[3] * cx + m[7] * cy + m[11] * cz + m[15];
    if (cw <= 1e-6f)
      continue;
    const float sx = (0.5f + 0.5f * (m[0] * cx + m[4] * cy + m[8] * cz + m[12]) / cw) * (float) w;
    const float sy = (0.5f + 0.5f * (m[1] * cx + m[5] * cy + m[9] * cz + m[13]) / cw) * (float) h;
    const float age = now - wv.t0;
    const float grow = 1.0f - std::exp (-age * 2.6f);
    const float radius = 6.0f + scr * (0.06f + 0.16f * wv.amp) * grow;
    const float width = 2.0f + 0.012f * scr * (0.4f + age);
    const float fade_in = std::min (1.0f, age / 0.05f);
    const float alpha = gain * wv.amp * fade_in * std::exp (-age * 2.4f) * 1.6f;
    if (alpha < 0.003f)
      continue;
    const float e = radius + 3.0f * width;
    const float corners[6][2] = {{-e, -e}, {e, -e}, {e, e}, {-e, -e}, {e, e}, {-e, e}};
    for (const auto &c : corners) {
      const float v[10] = {sx + c[0], sy + c[1], sx, sy, radius, width, 1.0f, 0.36f, 0.16f, alpha};
      verts_.insert (verts_.end (), v, v + 10);
    }
  }
  if (verts_.empty ())
    return;
  glEnable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_ONE);
  glUseProgram (prog_);
  glUniform2f (glGetUniformLocation (prog_, "uSize"), (float) w, (float) h);
  glBindVertexArray (vao_);
  glBindBuffer (GL_ARRAY_BUFFER, vbo_);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (verts_.size () * sizeof (float)), verts_.data (), GL_STREAM_DRAW);
  glDrawArrays (GL_TRIANGLES, 0, (GLsizei) (verts_.size () / 10));
  glBindVertexArray (0);
  glUseProgram (0);
}
