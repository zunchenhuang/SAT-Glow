#include "bloom.hpp"

#include <algorithm>

#include "glutil.hpp"

namespace {

// A single triangle that covers the whole viewport; no vertex buffer.
const char *FULL_VS = R"(#version 330 core
out vec2 vUV;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUV = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Downsample by two with five bilinear taps.  On the first level a soft
// threshold keeps only the bright parts.
const char *DOWN_FS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uSrc;
uniform vec2 uTexel;      // texel size of the source
uniform float uThreshold; // < 0: no threshold
out vec4 oCol;
vec3 bright(vec3 c) {
  if (uThreshold < 0.0) return c;
  float l = max(c.r, max(c.g, c.b));
  float knee = 0.5 * uThreshold;
  float soft = clamp(l - uThreshold + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee + 1e-4);
  return c * max(soft, l - uThreshold) / max(l, 1e-4);
}
void main() {
  vec2 h = uTexel;
  vec3 c = bright(texture(uSrc, vUV).rgb) * 4.0;
  c += bright(texture(uSrc, vUV + vec2(-h.x, -h.y)).rgb);
  c += bright(texture(uSrc, vUV + vec2( h.x, -h.y)).rgb);
  c += bright(texture(uSrc, vUV + vec2(-h.x,  h.y)).rgb);
  c += bright(texture(uSrc, vUV + vec2( h.x,  h.y)).rgb);
  oCol = vec4(c / 8.0, 1.0);
}
)";

// Upsample by two with a tent filter; added onto the larger level.
const char *UP_FS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uSrc;
uniform vec2 uTexel; // texel size of the source
out vec4 oCol;
void main() {
  vec2 h = uTexel;
  vec3 c = texture(uSrc, vUV + vec2(-2.0 * h.x, 0.0)).rgb;
  c += texture(uSrc, vUV + vec2(2.0 * h.x, 0.0)).rgb;
  c += texture(uSrc, vUV + vec2(0.0, -2.0 * h.y)).rgb;
  c += texture(uSrc, vUV + vec2(0.0, 2.0 * h.y)).rgb;
  c += texture(uSrc, vUV + vec2(-h.x, -h.y)).rgb * 2.0;
  c += texture(uSrc, vUV + vec2(h.x, -h.y)).rgb * 2.0;
  c += texture(uSrc, vUV + vec2(-h.x, h.y)).rgb * 2.0;
  c += texture(uSrc, vUV + vec2(h.x, h.y)).rgb * 2.0;
  oCol = vec4(c / 12.0, 1.0);
}
)";

// Scene + bloom, then a tone curve that is linear up to 0.8 and rolls off
// smoothly towards 1 instead of clipping.
const char *OUT_FS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uStrength; // 0: no bloom
out vec4 oCol;
vec3 tone(vec3 x) {
  vec3 hi = 0.8 + 0.2 * (1.0 - exp(-(x - 0.8) / 0.2));
  return mix(x, hi, step(0.8, x));
}
void main() {
  vec3 c = texture(uScene, vUV).rgb;
  if (uStrength > 0.0)
    c += uStrength * texture(uBloom, vUV).rgb;
  oCol = vec4(tone(c), 1.0);
}
)";

} // namespace

void Bloom::init () {
  prog_down_ = make_program (FULL_VS, DOWN_FS);
  prog_up_ = make_program (FULL_VS, UP_FS);
  prog_out_ = make_program (FULL_VS, OUT_FS);
  glGenVertexArrays (1, &vao_);
}

void Bloom::make_target (Level &l, int w, int h) {
  l.w = w;
  l.h = h;
  glGenTextures (1, &l.tex);
  glBindTexture (GL_TEXTURE_2D, l.tex);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers (1, &l.fbo);
  glBindFramebuffer (GL_FRAMEBUFFER, l.fbo);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
  glBindTexture (GL_TEXTURE_2D, 0);
}

void Bloom::free_target (Level &l) {
  if (l.fbo)
    glDeleteFramebuffers (1, &l.fbo);
  if (l.tex)
    glDeleteTextures (1, &l.tex);
  l = Level ();
}

void Bloom::resize (int w, int h) {
  if (w == w_ && h == h_)
    return;
  free_target (scene_);
  for (Level &l : mips_)
    free_target (l);
  mips_.clear ();
  w_ = w;
  h_ = h;
  make_target (scene_, w, h);
  int mw = w, mh = h;
  for (int i = 0; i < 6 && mw > 8 && mh > 8; i++) {
    mw = std::max (1, mw / 2);
    mh = std::max (1, mh / 2);
    Level l;
    make_target (l, mw, mh);
    mips_.push_back (l);
  }
  glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

void Bloom::begin (int w, int h, float r, float g, float b) {
  resize (std::max (1, w), std::max (1, h));
  glBindFramebuffer (GL_FRAMEBUFFER, scene_.fbo);
  glViewport (0, 0, w_, h_);
  glClearColor (r, g, b, 1.0f);
  glClear (GL_COLOR_BUFFER_BIT);
}

void Bloom::quad () {
  glBindVertexArray (vao_);
  glDrawArrays (GL_TRIANGLES, 0, 3);
}

void Bloom::finish (int vx, int vy, int vw, int vh, bool enabled, float strength, float threshold) {
  glDisable (GL_BLEND);
  glDisable (GL_DEPTH_TEST);
  const bool bloom = enabled && strength > 0 && !mips_.empty ();
  if (bloom) {
    // Down the chain.
    glUseProgram (prog_down_);
    glUniform1i (glGetUniformLocation (prog_down_, "uSrc"), 0);
    glActiveTexture (GL_TEXTURE0);
    const Level *src = &scene_;
    for (size_t i = 0; i < mips_.size (); i++) {
      glBindFramebuffer (GL_FRAMEBUFFER, mips_[i].fbo);
      glViewport (0, 0, mips_[i].w, mips_[i].h);
      glBindTexture (GL_TEXTURE_2D, src->tex);
      glUniform2f (glGetUniformLocation (prog_down_, "uTexel"), 1.0f / (float) src->w, 1.0f / (float) src->h);
      glUniform1f (glGetUniformLocation (prog_down_, "uThreshold"), i == 0 ? threshold : -1.0f);
      quad ();
      src = &mips_[i];
    }
    // And back up, adding each level onto the next larger one.
    glUseProgram (prog_up_);
    glUniform1i (glGetUniformLocation (prog_up_, "uSrc"), 0);
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE);
    for (size_t i = mips_.size () - 1; i > 0; i--) {
      glBindFramebuffer (GL_FRAMEBUFFER, mips_[i - 1].fbo);
      glViewport (0, 0, mips_[i - 1].w, mips_[i - 1].h);
      glBindTexture (GL_TEXTURE_2D, mips_[i].tex);
      glUniform2f (glGetUniformLocation (prog_up_, "uTexel"), 0.5f / (float) mips_[i].w, 0.5f / (float) mips_[i].h);
      quad ();
    }
    glDisable (GL_BLEND);
  }

  glBindFramebuffer (GL_FRAMEBUFFER, 0);
  glViewport (vx, vy, vw, vh);
  glUseProgram (prog_out_);
  glUniform1i (glGetUniformLocation (prog_out_, "uScene"), 0);
  glUniform1i (glGetUniformLocation (prog_out_, "uBloom"), 1);
  glUniform1f (glGetUniformLocation (prog_out_, "uStrength"), bloom ? strength : 0.0f);
  glActiveTexture (GL_TEXTURE1);
  glBindTexture (GL_TEXTURE_2D, bloom ? mips_[0].tex : scene_.tex);
  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, scene_.tex);
  quad ();
  glBindTexture (GL_TEXTURE_2D, 0);
  glBindVertexArray (0);
  glUseProgram (0);
}
