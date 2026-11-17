// SATGlow: watch a SAT solver work.
//
// Three threads:
//   solver thread  runs the hooked SAT solver and writes per-variable state
//   layout thread  keeps improving the force-directed graph layout
//   render thread  (this file) reads both once per frame and draws
#include <glad/glad.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "bloom.hpp"
#include "clusters.hpp"
#include "glutil.hpp"
#include "graph.hpp"
#include "intro.hpp"
#include "layout.hpp"
#include "logo.hpp"
#include "recorder.hpp"
#include "shockwave.hpp"
#include "solver.hpp"

/*------------------------------------------------------------------------*/
// Command line

struct Options {
  std::string file;
  int solver = 0;        // index into SOLVER_NAMES
  bool three_d = false;  // start with the 3D graph
  bool intro = true;     // play the start-up animation
  std::string logo_svg;  // write the logo here and exit
  std::string replay;    // replay this trace instead of solving
  int clusters = -1;     // -1 automatic (large formulas), 0 off, 1 on
  bool bloom = true;
  bool waves = true;
  std::string record;    // record a video / GIF from the start ...
  double record_seconds = 0; // ... of this length (0: until the window closes)
  int fps = 30;
  double speed = -1; // events per second, < 0 picks a default from the size
  bool unlimited = false;
  bool paused = false;
  bool fullscreen = false;
  bool edges_set = false, edges = true;
  int width = 1600, height = 900;
  float ui_scale = 1.0f;
  std::string shot; // write a screenshot here ...
  double shot_after = 5.0; // ... after this many seconds, then exit
  std::vector<std::pair<std::string, int>> solver_options;
};

static void usage () {
  printf (
      "usage: satglow [options] FILE.cnf[.xz|.gz|.bz2]\n"
      "\n"
      "  --solver NAME    cadical (default), kissat or minisat\n"
      "  --replay TRACE   replay a trace recorded with satglow-check --trace\n"
      "  --3d             start with the graph in 3D\n"
      "  --no-intro       skip the start-up animation\n"
      "  --clusters       group variables into clusters (default above 20,000)\n"
      "  --no-clusters    never group variables into clusters\n"
      "  --no-bloom       no bloom (glow post-processing)\n"
      "  --no-waves       no conflict shockwaves\n"
      "  --speed N        solver events per second (assignments and\n"
      "                   unassignments); default depends on instance size\n"
      "  --max            run the solver at full speed\n"
      "  --paused         start paused (SPACE resumes, S single-steps)\n"
      "  --no-edges       do not draw graph edges\n"
      "  --size WxH       window size (default 1600x900)\n"
      "  --fullscreen     use the whole primary monitor\n"
      "  --ui-scale F     scale the side panel text (default 1.0)\n"
      "  --opt NAME=VAL   pass an option to the solver, e.g. --opt chrono=0\n"
      "  --shot FILE.ppm  save a screenshot and exit (see --shot-after)\n"
      "  --shot-after S   seconds to wait before --shot (default 5)\n"
      "  --logo-svg FILE  write the SATGlow logo as an SVG file and exit\n"
      "  --record FILE    record a video from the start: .mp4, .webm or .gif\n"
      "                   (needs ffmpeg).  Every frame advances the animation\n"
      "                   by exactly 1/fps, so the video is smooth even if\n"
      "                   the window runs slower\n"
      "  --record-seconds S  stop recording and exit after S seconds of video\n"
      "  --fps N          frame rate of recordings (default 30)\n"
      "  -h, --help       this text\n");
}

static bool parse_args (int argc, char **argv, Options &o) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&] (const char *what) -> const char * {
      if (i + 1 >= argc) {
        fprintf (stderr, "satglow: %s needs an argument\n", what);
        exit (2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage ();
      exit (0);
    } else if (a == "--solver") {
      const char *name = need ("--solver");
      o.solver = solver_index (name);
      if (o.solver < 0) {
        fprintf (stderr, "satglow: unknown solver '%s' (cadical, kissat, minisat)\n", name);
        return false;
      }
    } else if (a == "--3d")
      o.three_d = true;
    else if (a == "--no-intro")
      o.intro = false;
    else if (a == "--clusters")
      o.clusters = 1;
    else if (a == "--no-clusters")
      o.clusters = 0;
    else if (a == "--no-bloom")
      o.bloom = false;
    else if (a == "--no-waves")
      o.waves = false;
    else if (a == "--record")
      o.record = need ("--record");
    else if (a == "--record-seconds")
      o.record_seconds = atof (need ("--record-seconds"));
    else if (a == "--fps")
      o.fps = std::max (1, std::min (120, atoi (need ("--fps"))));
    else if (a == "--logo-svg")
      o.logo_svg = need ("--logo-svg");
    else if (a == "--replay")
      o.replay = need ("--replay");
    else if (a == "--speed")
      o.speed = atof (need ("--speed"));
    else if (a == "--max")
      o.unlimited = true;
    else if (a == "--paused")
      o.paused = true;
    else if (a == "--no-edges")
      o.edges_set = true, o.edges = false;
    else if (a == "--fullscreen")
      o.fullscreen = true;
    else if (a == "--ui-scale")
      o.ui_scale = (float) atof (need ("--ui-scale"));
    else if (a == "--size") {
      if (sscanf (need ("--size"), "%dx%d", &o.width, &o.height) != 2) {
        fprintf (stderr, "satglow: --size expects WxH\n");
        return false;
      }
    } else if (a == "--opt") {
      const char *s = need ("--opt");
      const char *eq = strchr (s, '=');
      if (!eq) {
        fprintf (stderr, "satglow: --opt expects NAME=VALUE\n");
        return false;
      }
      o.solver_options.push_back (
          {std::string (s, (size_t) (eq - s)), atoi (eq + 1)});
    } else if (a == "--shot")
      o.shot = need ("--shot");
    else if (a == "--shot-after")
      o.shot_after = atof (need ("--shot-after"));
    else if (!a.empty () && a[0] == '-') {
      fprintf (stderr, "satglow: unknown option '%s' (try --help)\n",
               a.c_str ());
      return false;
    } else
      o.file = a;
  }
  if (o.file.empty () && o.logo_svg.empty ()) {
    usage ();
    return false;
  }
  return true;
}

/*------------------------------------------------------------------------*/
// Shaders

static const char *NODE_VS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in float aSize;
layout(location = 3) in float aGain; // < 1 dims nodes packed tightly in a cluster
uniform mat4 uMVP;
uniform float uRefW;       // clip-space w of a point at the camera target
uniform float uPointScale;
uniform float uMaxPoint;
out vec4 vCol;
void main() {
  gl_Position = uMVP * vec4(aPos, 1.0);
  // Perspective: nearer nodes are bigger and brighter.  In the flat 2D view
  // every node has the same depth and att is exactly 1.
  float att = uRefW / max(gl_Position.w, 0.05 * uRefW);
  // The sprite is three times the size of the bright core, the rest is halo.
  gl_PointSize = clamp(aSize * uPointScale * 3.0 * att, 1.0, uMaxPoint);
  vCol = vec4(aCol.rgb * clamp(pow(att, 1.6), 0.22, 1.7) * aGain, 1.0);
}
)";

static const char *NODE_FS = R"(#version 330 core
in vec4 vCol;
uniform float uGlow;
uniform float uBright;
out vec4 oCol;
void main() {
  vec2 p = gl_PointCoord * 2.0 - 1.0;
  float d2 = dot(p, p);
  if (d2 > 1.0) discard;
  float d = sqrt(d2);
  float core = smoothstep(0.40, 0.26, d);
  float halo = exp(-d2 * 5.0) * (1.0 - d);
  oCol = vec4(vCol.rgb * uBright * (core * 0.85 + halo * uGlow), 1.0);
}
)";

static const char *EDGE_VS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aCol;
uniform mat4 uMVP;
uniform float uRefW;
out vec4 vCol;
void main() {
  gl_Position = uMVP * vec4(aPos, 1.0);
  float att = uRefW / max(gl_Position.w, 0.05 * uRefW);
  vCol = vec4(aCol.rgb * clamp(pow(att, 1.6), 0.22, 1.5), 1.0);
}
)";

static const char *EDGE_FS = R"(#version 330 core
in vec4 vCol;
uniform float uAlpha;
out vec4 oCol;
void main() {
  vec3 c = mix(vec3(0.22, 0.26, 0.40), vCol.rgb, 0.8);
  oCol = vec4(c * uAlpha, 1.0);
}
)";

/*------------------------------------------------------------------------*/
// A little matrix code for the camera (column-major, like OpenGL).

struct Mat4 {
  float m[16];
};

static Mat4 mat_mul (const Mat4 &a, const Mat4 &b) {
  Mat4 r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float v = 0;
      for (int k = 0; k < 4; k++)
        v += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = v;
    }
  return r;
}

static Mat4 mat_perspective (float fovy, float aspect, float znear, float zfar) {
  const float f = 1.0f / std::tan (0.5f * fovy);
  Mat4 r = {};
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = (zfar + znear) / (znear - zfar);
  r.m[11] = -1.0f;
  r.m[14] = 2.0f * zfar * znear / (znear - zfar);
  return r;
}

// View matrix from the camera's position and its right / up / back axes.
static Mat4 mat_view (const float eye[3], const float right[3], const float up[3], const float back[3]) {
  Mat4 r = {};
  for (int i = 0; i < 3; i++) {
    r.m[i * 4 + 0] = right[i];
    r.m[i * 4 + 1] = up[i];
    r.m[i * 4 + 2] = back[i];
  }
  r.m[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
  r.m[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
  r.m[14] = -(back[0] * eye[0] + back[1] * eye[1] + back[2] * eye[2]);
  r.m[15] = 1.0f;
  return r;
}

static const float FOVY = 0.60f; // vertical field of view of the graph camera

/*------------------------------------------------------------------------*/
// Colours

struct RGB {
  float r, g, b;
};

static inline RGB mix (RGB a, RGB b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
static inline RGB scale (RGB a, float s) { return {a.r * s, a.g * s, a.b * s}; }
static inline RGB add (RGB a, RGB b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }

static const RGB COL_UNASSIGNED = {0.13f, 0.16f, 0.26f};
static const RGB COL_GONE = {0.045f, 0.075f, 0.075f}; // eliminated etc.
static const RGB COL_TRUE = {1.00f, 0.58f, 0.10f};  // amber
static const RGB COL_FALSE = {0.10f, 0.62f, 1.00f}; // blue
static const RGB COL_WHITE = {1.0f, 1.0f, 1.0f};
static const RGB COL_AFTERGLOW = {0.55f, 0.18f, 0.80f}; // just unassigned
static const RGB COL_CONFLICT = {1.0f, 0.08f, 0.16f};   // in a learned clause

static const RGB CL_SAT = {0.04f, 0.26f, 0.12f};
static const RGB CL_OPEN = {0.10f, 0.11f, 0.15f};
static const RGB CL_TIGHT = {0.80f, 0.45f, 0.08f};
static const RGB CL_UNIT = {1.0f, 0.92f, 0.25f};
static const RGB CL_FALSIFIED = {1.0f, 0.10f, 0.10f};
static const RGB CL_REMOVED = {0.035f, 0.05f, 0.06f}; // has an eliminated variable

// Piecewise-linear colour ramps.
static RGB ramp (const RGB *stops, int n, float t) {
  t = std::min (1.0f, std::max (0.0f, t)) * (float) (n - 1);
  int i = std::min (n - 2, (int) t);
  return mix (stops[i], stops[i + 1], t - (float) i);
}
static RGB level_colour (float t) {
  static const RGB stops[] = {{0.25f, 0.35f, 1.00f},
                              {0.10f, 0.80f, 0.95f},
                              {0.20f, 0.95f, 0.40f},
                              {0.98f, 0.90f, 0.20f},
                              {1.00f, 0.35f, 0.15f}};
  return ramp (stops, 5, t);
}
static RGB activity_colour (float t) {
  static const RGB stops[] = {{0.06f, 0.06f, 0.12f},
                              {0.33f, 0.10f, 0.50f},
                              {0.80f, 0.25f, 0.33f},
                              {0.98f, 0.62f, 0.12f},
                              {1.00f, 0.98f, 0.70f}};
  return ramp (stops, 5, t);
}

static inline void put (uint8_t *p, RGB c) {
  p[0] = (uint8_t) (std::min (1.0f, std::max (0.0f, c.r)) * 255.0f + 0.5f);
  p[1] = (uint8_t) (std::min (1.0f, std::max (0.0f, c.g)) * 255.0f + 0.5f);
  p[2] = (uint8_t) (std::min (1.0f, std::max (0.0f, c.b)) * 255.0f + 0.5f);
  p[3] = 255;
}

/*------------------------------------------------------------------------*/
// Application state

enum { VIEW_SPLIT = 0, VIEW_GRAPH = 1, VIEW_GRIDS = 2 };
enum { COLOUR_VALUE = 0, COLOUR_LEVEL = 1, COLOUR_ACTIVITY = 2 };

static const int HISTORY = 400;   // samples kept for the plots
static const int LEVEL_BINS = 96; // bars in the decision level histogram

struct Rect {
  float x, y, w, h;
  bool contains (float px, float py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

struct App {
  Options opt;
  Cnf cnf;
  Graph graph;
  Shared shared;
  SolverRunner runner;
  Layout layout;
  Intro intro;
  int solver = 0;              // index into SOLVER_NAMES
  bool solver_started = false; // the solver waits for the intro to finish
  float intro_t = 0;           // seconds into the intro, >= Intro::END when over

  // speed control
  double rate = 200.0;
  double token_acc = 0;
  bool unlimited = false;
  bool paused = false;

  // per-variable render state (index = variable - 1)
  std::vector<float> pos;        // x, y, z from the layout thread
  std::vector<uint8_t> col;      // RGBA8, padded to the variable grid size
  std::vector<float> size;       // relative point size
  std::vector<float> heat;       // 1 right after a flip, decays to 0
  std::vector<float> cheat;      // same for "occurred in a learned clause"
  std::vector<uint32_t> prev_flips, prev_learnt;
  uint32_t max_flips = 1;
  float flip_norm = 1.0f, learn_norm = 1.0f; // busiest variable per frame

  // clause grid
  std::vector<uint8_t> clause_col;
  size_t clause_cursor = 0;
  int grid_w = 1, grid_h = 1, cgrid_w = 1, cgrid_h = 1;

  // GL objects
  GLuint prog_node = 0, prog_edge = 0;
  GLuint vao = 0, vbo_pos = 0, vbo_col = 0, vbo_size = 0, vbo_gain = 0, ibo = 0;
  std::vector<float> gain; // per node, see aGain in the node shader
  GLuint tex_vars = 0, tex_clauses = 0;
  float max_point = 64.0f;

  // view
  int view = VIEW_SPLIT;
  int colour_mode = COLOUR_VALUE;
  float node_size = 6.0f;
  float glow = 0.6f;
  float brightness = 1.0f;
  bool show_edges = true;
  float edge_alpha = 0.1f;
  bool auto_fit = true;
  bool view3d = false;      // graph in three dimensions
  bool auto_rotate = true;  // slowly turn the 3D graph
  float cam_t[3] = {0, 0, 0}; // point the camera looks at
  float cam_dist = 1.0f, fit_dist = 1.0f; // distance to it; distance that fits the graph
  float yaw = 0, pitch = 0, pitch_goal = 0.42f;
  bool cam_init = false;
  int dragging = 0; // 0 no, 1 left button (pan in 2D, orbit in 3D), 2 right button (pan)
  Mat4 mvp = {};

  // statistics for the panel
  float h_trail[HISTORY] = {}, h_level[HISTORY] = {}, h_conf[HISTORY] = {},
        h_events[HISTORY] = {};
  int h_pos = 0;
  double h_timer = 0;
  uint64_t last_events = 0, last_conflicts = 0;
  double events_per_s = 0, conflicts_per_s = 0;
  float level_hist[LEVEL_BINS] = {};
  int level_hist_bins = 1;
  double run_clock = 0; // seconds the solver has been in ST_RUNNING
  int screenshot_counter = 0;
  float update_ms = 0; // CPU time per frame spent reading solver state

  // animation clock (seconds); follows real time, or video time when
  // recording offline
  float now = 0;

  // bloom
  Bloom bloom;
  bool bloom_on = true;
  float bloom_strength = 0.9f;

  // conflict shockwaves
  Shockwaves waves;
  bool waves_on = true;
  float wave_gain = 1.0f;
  uint64_t last_clause_seq = 0;
  float last_wave_t = -10.0f;
  std::vector<float> boost;

  // clustering
  Clustering clu;
  bool clu_ready = false, clustered = false;
  std::vector<float> cpos;   // cluster centres from the layout thread
  std::vector<uint8_t> ccol; // cluster colours (RGBA8), for cluster edges
  std::vector<float> csum;
  std::vector<float> crad; // cluster radii (layout units)
  float depth_mix = 0;       // 0 flat .. 1 three-dimensional
  float cluster_spread = 1.0f;
  GLuint cvao = 0, cvbo_pos = 0, cvbo_col = 0, cibo = 0;

  // recording
  Recorder rec;
  int rec_format = 0; // 0 mp4, 1 gif, 2 webm
  bool offline = false; // recording from the command line: fixed time step
  double rec_acc = 0;
  bool want_rec_toggle = false, want_shot = false;
  std::string title;

  // short message at the bottom of the panel
  std::string msg;
  double msg_until = 0;
};

static double wall_seconds () {
  return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

static void notify (App &a, const std::string &m) {
  printf ("satglow: %s\n", m.c_str ());
  a.msg = m;
  a.msg_until = wall_seconds () + 5.0;
}

/*------------------------------------------------------------------------*/

static double default_rate (int nvars) {
  if (nvars <= 100)
    return 40;
  if (nvars <= 1000)
    return 400;
  if (nvars <= 10000)
    return 5000;
  return 100000;
}

static void restart_solver (App &a) {
  a.runner.start (a.cnf, a.shared, a.solver, a.opt.solver_options);
  a.solver_started = true;
  a.waves.clear ();
  a.last_clause_seq = 0;
  std::fill (a.heat.begin (), a.heat.end (), 0.0f);
  std::fill (a.cheat.begin (), a.cheat.end (), 0.0f);
  std::fill (a.prev_flips.begin (), a.prev_flips.end (), 0u);
  std::fill (a.prev_learnt.begin (), a.prev_learnt.end (), 0u);
  a.max_flips = 1;
  a.flip_norm = a.learn_norm = 1.0f;
  a.last_events = a.last_conflicts = 0;
  a.run_clock = 0;
  a.token_acc = 0;
}

static void init_gl (App &a) {
  const size_t n = (size_t) a.cnf.nvars;
  a.prog_node = make_program (NODE_VS, NODE_FS);
  a.prog_edge = make_program (EDGE_VS, EDGE_FS);

  glGenVertexArrays (1, &a.vao);
  glBindVertexArray (a.vao);

  glGenBuffers (1, &a.vbo_pos);
  glBindBuffer (GL_ARRAY_BUFFER, a.vbo_pos);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (n * 3 * sizeof (float)), nullptr,
                GL_STREAM_DRAW);
  glEnableVertexAttribArray (0);
  glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glGenBuffers (1, &a.vbo_col);
  glBindBuffer (GL_ARRAY_BUFFER, a.vbo_col);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (n * 4), nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray (1);
  glVertexAttribPointer (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);

  glGenBuffers (1, &a.vbo_size);
  glBindBuffer (GL_ARRAY_BUFFER, a.vbo_size);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (n * sizeof (float)), nullptr,
                GL_STREAM_DRAW);
  glEnableVertexAttribArray (2);
  glVertexAttribPointer (2, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

  a.gain.assign (n, 1.0f);
  glGenBuffers (1, &a.vbo_gain);
  glBindBuffer (GL_ARRAY_BUFFER, a.vbo_gain);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (n * sizeof (float)), a.gain.data (), GL_STREAM_DRAW);
  glEnableVertexAttribArray (3);
  glVertexAttribPointer (3, 1, GL_FLOAT, GL_FALSE, 0, nullptr);

  glGenBuffers (1, &a.ibo);
  glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, a.ibo);
  glBufferData (GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr) (a.graph.edges.size () * sizeof (uint32_t)),
                a.graph.edges.data (), GL_STATIC_DRAW);
  glBindVertexArray (0);

  auto make_tex = [] (int w, int h) {
    GLuint t;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, nullptr);
    return t;
  };
  a.tex_vars = make_tex (a.grid_w, a.grid_h);
  a.tex_clauses = make_tex (a.cgrid_w, a.cgrid_h);

  GLfloat range[2] = {1.0f, 64.0f};
  glGetFloatv (GL_POINT_SIZE_RANGE, range);
  a.max_point = range[1] > 1.0f ? range[1] : 64.0f;
}

/*------------------------------------------------------------------------*/
// Once per frame: turn the solver's per-variable state into colours.

static void update_variables (App &a, float dt) {
  const int n = a.cnf.nvars;
  const uint32_t *state = a.shared.state.data ();
  const uint32_t *flips = a.shared.flips.data ();
  const uint32_t *learnt = a.shared.learnt.data ();
  const float decay = std::exp (-dt / 0.35f);
  const float cdecay = std::exp (-dt / 0.70f);
  const int max_level = std::max (1, SG_LOAD (&a.shared.ctx.max_level));
  const float inv_log_max = 1.0f / std::log1p ((float) a.max_flips);

  a.level_hist_bins = std::min (LEVEL_BINS, max_level + 1);
  std::fill (a.level_hist, a.level_hist + LEVEL_BINS, 0.0f);
  uint32_t max_flips = a.max_flips;

  // Heat is relative to the busiest variable of the frame.  At slow speeds
  // that is a single flip, so every event flashes; at full speed it picks
  // out where the solver is working hardest.
  const float inv_flip = 1.0f / a.flip_norm, inv_learn = 1.0f / a.learn_norm;
  uint32_t frame_flips = 0, frame_learnt = 0;

  for (int i = 0; i < n; i++) {
    const int v = i + 1;
    const uint32_t w = SG_LOAD (&state[v]);
    const uint32_t f = SG_LOAD (&flips[v]);
    const uint32_t l = SG_LOAD (&learnt[v]);
    const uint32_t val = w & SG_VAL_MASK;
    const uint32_t kind = (w >> SG_KIND_SHIFT) & SG_KIND_MASK;
    const int level = (int) (w >> SG_LEVEL_SHIFT);

    float h = a.heat[i] * decay;
    if (f != a.prev_flips[i]) {
      const uint32_t d = f - a.prev_flips[i];
      h = std::max (h, std::sqrt (std::min (1.0f, (float) d * inv_flip)));
      a.prev_flips[i] = f;
      if (d > frame_flips)
        frame_flips = d;
      if (f > max_flips)
        max_flips = f;
    }
    a.heat[i] = h;
    float ch = a.cheat[i] * cdecay;
    if (l != a.prev_learnt[i]) {
      const uint32_t d = l - a.prev_learnt[i];
      ch = std::max (ch, std::min (1.0f, (float) d * inv_learn));
      a.prev_learnt[i] = l;
      if (d > frame_learnt)
        frame_learnt = d;
    }
    a.cheat[i] = ch;

    RGB c;
    float s;
    if (!val) {
      if (w & SG_GONE) {
        // Removed by preprocessing: the search never touches it again.
        c = COL_GONE;
        s = 0.6f;
      } else {
        // Unassigned: dark, with a violet afterglow right after backtracking.
        c = add (COL_UNASSIGNED, scale (COL_AFTERGLOW, h * 0.9f));
        s = 0.8f + 0.25f * h;
      }
    } else {
      if (level > 0 && kind != SG_KIND_MODEL) {
        int bin = a.level_hist_bins == max_level + 1
                      ? level
                      : (int) ((int64_t) level * a.level_hist_bins / (max_level + 1));
        a.level_hist[std::min (a.level_hist_bins - 1, std::max (0, bin))] += 1.0f;
      }
      RGB base;
      if (a.colour_mode == COLOUR_LEVEL)
        base = level_colour ((float) level / (float) max_level);
      else
        base = val == SG_VAL_TRUE ? COL_TRUE : COL_FALSE;
      float brightness = 0.45f + 0.55f * h;
      s = 1.0f + 0.35f * h;
      if (kind == SG_KIND_MODEL) {
        brightness = 0.55f; // filled in from the final model
      } else if (level == 0) {
        base = mix (base, COL_WHITE, 0.35f); // fixed for good: pale and calm
        brightness = 0.36f + 0.45f * h;
      } else if (kind == SG_KIND_DECISION) {
        base = mix (base, COL_WHITE, 0.45f);
        brightness = 0.85f + 0.15f * h;
        s = 1.7f + 0.3f * h;
      }
      c = scale (base, brightness);
    }
    if (a.colour_mode == COLOUR_ACTIVITY) {
      // How often has this variable been assigned so far (log scale)?
      c = activity_colour (std::log1p ((float) f) * inv_log_max);
      if (val)
        c = mix (c, COL_WHITE, 0.25f);
      s = 0.9f + 0.3f * h;
    }
    if (ch > 0.01f) {
      c = mix (c, COL_CONFLICT, ch * 0.9f);
      s += 0.8f * ch;
    }
    put (&a.col[(size_t) i * 4], c);
    a.size[i] = s;
  }
  a.max_flips = max_flips;
  a.flip_norm = std::max (1.0f, a.flip_norm + 0.25f * ((float) frame_flips - a.flip_norm));
  a.learn_norm = std::max (1.0f, a.learn_norm + 0.25f * ((float) frame_learnt - a.learn_norm));
}

// Clause colours are recomputed in slices so that huge formulas do not stall
// a frame; small formulas are covered completely every frame.
static void update_clauses (App &a) {
  const size_t m = a.cnf.nclauses ();
  if (!m)
    return;
  const uint32_t *state = a.shared.state.data ();
  size_t budget = 2000000; // literals per frame
  size_t done = 0;
  size_t i = a.clause_cursor;
  while (done < m) {
    const int *p = a.cnf.begin (i), *e = a.cnf.end (i);
    int unassigned = 0, falsified = 0;
    bool sat = false, removed = false;
    for (const int *q = p; q != e; q++) {
      const uint32_t w = SG_LOAD (&state[*q < 0 ? -*q : *q]);
      const uint32_t val = w & SG_VAL_MASK;
      if (!val) {
        unassigned++;
        if (w & SG_GONE)
          removed = true;
      }
      else if ((val == SG_VAL_TRUE) == (*q > 0)) {
        sat = true;
        break;
      } else
        falsified++;
    }
    RGB c;
    if (sat)
      c = CL_SAT;
    else if (removed)
      c = CL_REMOVED; // the solver replaced it when it eliminated a variable
    else if (!unassigned)
      c = CL_FALSIFIED;
    else if (unassigned == 1 && falsified > 0)
      c = CL_UNIT;
    else
      c = mix (CL_OPEN, CL_TIGHT,
               0.8f * (float) falsified / (float) (falsified + unassigned));
    put (&a.clause_col[i * 4], c);
    const size_t len = (size_t) (e - p) + 1;
    done++;
    if (++i == m)
      i = 0;
    if (len >= budget)
      break;
    budget -= len;
  }
  a.clause_cursor = i;
}

/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
// Clusters

static void set_clustered (App &a, bool on) {
  if (on == a.clustered)
    return;
  const size_t n = (size_t) a.cnf.nvars;
  if (on && !a.clu_ready) {
    build_clustering (a.graph, a.clu);
    a.clu_ready = true;
    char m[160];
    snprintf (m, sizeof m, "%d clusters (modularity %.3f) in %.2f s", a.clu.k, a.clu.modularity, a.clu.seconds);
    notify (a, m);
    glGenVertexArrays (1, &a.cvao);
    glBindVertexArray (a.cvao);
    glGenBuffers (1, &a.cvbo_pos);
    glBindBuffer (GL_ARRAY_BUFFER, a.cvbo_pos);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ((size_t) a.clu.k * 3 * sizeof (float)), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers (1, &a.cvbo_col);
    glBindBuffer (GL_ARRAY_BUFFER, a.cvbo_col);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ((size_t) a.clu.k * 4), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
    glGenBuffers (1, &a.cibo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, a.cibo);
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (a.clu.cgraph.edges.size () * sizeof (uint32_t)),
                  a.clu.cgraph.edges.data (), GL_STATIC_DRAW);
    glBindVertexArray (0);
    a.ccol.assign ((size_t) a.clu.k * 4, 0);
  }
  const bool have_pos = a.pos.size () == 3 * n;
  if (on) {
    // Start each cluster where its members are now.
    std::vector<float> init ((size_t) a.clu.k * 3, 0.0f);
    if (have_pos) {
      for (size_t v = 0; v < n; v++)
        for (int d = 0; d < 3; d++)
          init[3 * (size_t) a.clu.of[v] + (size_t) d] += a.pos[3 * v + (size_t) d];
      for (int c = 0; c < a.clu.k; c++)
        for (int d = 0; d < 3; d++)
          init[3 * (size_t) c + (size_t) d] /= (float) a.clu.size[(size_t) c];
    }
    a.layout.start (&a.clu.cgraph, 777u, a.view3d, have_pos ? &init : nullptr);
    a.edge_alpha = 0.30f;
    a.show_edges = true;
  } else {
    // Start every variable where it is drawn now.
    a.layout.start (&a.graph, 12345u, a.view3d, have_pos ? &a.pos : nullptr);
    a.edge_alpha = std::min (0.35f, std::max (0.012f, 22.0f / std::sqrt ((float) a.graph.nedges () + 1.0f)));
  }
  a.clustered = on;
}

/*------------------------------------------------------------------------*/
// Shockwaves

static void update_waves (App &a) {
  const sg_ctx &c = a.shared.ctx;
  const uint64_t seq = SG_LOAD (&c.clause_seq);
  // At most one new wave every 0.12 s; at full speed that picks out a
  // sample of the conflicts.
  if (a.waves_on && seq != a.last_clause_seq && a.now - a.last_wave_t >= 0.12f) {
    int len = SG_LOAD (&c.clause_len);
    len = std::max (0, std::min (SG_CLAUSE_MAX, len));
    int32_t vars[SG_CLAUSE_MAX];
    for (int i = 0; i < len; i++)
      vars[i] = SG_LOAD (&c.clause_vars[i]);
    const int jump = SG_LOAD (&c.last_jump_from) - SG_LOAD (&c.last_jump_to);
    const float strength = 0.5f + 0.5f * std::min (1.0f, (float) std::max (0, jump - 1) / 6.0f);
    a.waves.spawn (vars, len, strength, a.now);
    a.last_wave_t = a.now;
    a.last_clause_seq = seq;
  }
  a.waves.update (a.now, a.boost);
  if (!a.waves.running ())
    return;
  // Wave light: heat the variables up towards white-hot orange.
  const size_t n = (size_t) a.cnf.nvars;
  for (size_t i = 0; i < n; i++) {
    const float b = std::min (1.0f, a.boost[i] * a.wave_gain);
    if (b < 0.004f)
      continue;
    uint8_t *p = &a.col[i * 4];
    const float hot[3] = {255.0f, 175.0f, 110.0f};
    for (int k = 0; k < 3; k++)
      p[k] = (uint8_t) std::min (255.0f, (float) p[k] + (hot[k] - (float) p[k]) * b * 0.85f + 40.0f * b);
    a.size[i] += 1.3f * b;
  }
}

// Bounding box of the layout for fitting the camera.  A few stragglers
// (variables that occur in no clause, say) must not shrink everything else
// to a dot, so larger graphs use the 1% .. 99% range of a sample instead of
// the true extremes.
static void graph_bounds (const App &a, float lo[3], float hi[3]) {
  const size_t n = (size_t) a.cnf.nvars;
  static std::vector<float> v[3];
  const size_t step = std::max<size_t> (1, n / 8192);
  for (int d = 0; d < 3; d++) {
    v[d].clear ();
    for (size_t i = 0; i < n; i += step)
      v[d].push_back (a.pos[3 * i + (size_t) d]);
    const size_t k = v[d].size ();
    if (k < 200) {
      lo[d] = *std::min_element (v[d].begin (), v[d].end ());
      hi[d] = *std::max_element (v[d].begin (), v[d].end ());
      continue;
    }
    const size_t a0 = k / 100, a1 = k - 1 - k / 100;
    std::nth_element (v[d].begin (), v[d].begin () + (long) a0, v[d].end ());
    lo[d] = v[d][a0];
    std::nth_element (v[d].begin (), v[d].begin () + (long) a1, v[d].end ());
    hi[d] = v[d][a1];
    const float e = 0.12f * (hi[d] - lo[d]);
    lo[d] -= e;
    hi[d] += e;
  }
}

// Where a node lands on screen (window coordinates).  False if it is
// behind the camera.
static bool project_node (const App &a, const Rect &r, size_t i, float &sx, float &sy) {
  const float *p = &a.pos[3 * i];
  const float *m = a.mvp.m;
  const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
  if (w <= 1e-6f)
    return false;
  const float x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w;
  const float y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
  sx = r.x + (0.5f + 0.5f * x) * r.w;
  sy = r.y + (0.5f - 0.5f * y) * r.h;
  return true;
}

static void save_screenshot (const std::string &path, int w, int h) {
  std::vector<uint8_t> px ((size_t) w * h * 3);
  glPixelStorei (GL_PACK_ALIGNMENT, 1);
  glReadBuffer (GL_BACK);
  glReadPixels (0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data ());
  FILE *f = fopen (path.c_str (), "wb");
  if (!f) {
    fprintf (stderr, "satglow: cannot write '%s'\n", path.c_str ());
    return;
  }
  fprintf (f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--)
    fwrite (px.data () + (size_t) y * w * 3, 1, (size_t) w * 3, f);
  fclose (f);
  printf ("satglow: wrote %s\n", path.c_str ());
}

static const char *status_text (int st, bool paused) {
  switch (st) {
  case ST_LOADING:
    return paused ? "PAUSED" : "LOADING";
  case ST_RUNNING:
    return paused ? "PAUSED" : "RUNNING";
  case ST_SAT:
    return "SATISFIABLE";
  case ST_UNSAT:
    return "UNSATISFIABLE";
  case ST_ABORTED:
    return "ABORTED";
  default:
    return "IDLE";
  }
}

static std::string human (double v) {
  char b[64];
  if (v >= 1e9)
    snprintf (b, sizeof b, "%.2fG", v / 1e9);
  else if (v >= 1e6)
    snprintf (b, sizeof b, "%.2fM", v / 1e6);
  else if (v >= 1e4)
    snprintf (b, sizeof b, "%.1fk", v / 1e3);
  else
    snprintf (b, sizeof b, "%.0f", v);
  return b;
}

static void legend_item (const char *label, RGB c) {
  ImVec2 p = ImGui::GetCursorScreenPos ();
  float h = ImGui::GetTextLineHeight ();
  ImGui::GetWindowDrawList ()->AddRectFilled (
      ImVec2 (p.x, p.y + 1), ImVec2 (p.x + h - 2, p.y + h - 1),
      ImGui::GetColorU32 (ImVec4 (c.r, c.g, c.b, 1.0f)));
  ImGui::Dummy (ImVec2 (h, h));
  ImGui::SameLine ();
  ImGui::TextUnformatted (label);
}

// Draws a pixel grid texture into `area`, keeping square cells.  Returns the
// rectangle actually covered.
static Rect draw_grid (GLuint tex, int gw, int gh, Rect area, const char *title, float alpha) {
  ImDrawList *dl = ImGui::GetBackgroundDrawList ();
  const float pad = 8.0f, head = ImGui::GetTextLineHeight () + 6.0f;
  float aw = area.w - 2 * pad, ah = area.h - 2 * pad - head;
  if (aw < 8 || ah < 8)
    return {0, 0, 0, 0};
  float cell = std::min (aw / (float) gw, ah / (float) gh);
  if (cell >= 1.0f)
    cell = std::floor (cell); // crisp pixels when there is room
  Rect r;
  r.w = cell * (float) gw;
  r.h = cell * (float) gh;
  r.x = std::floor (area.x + pad + 0.5f * (aw - r.w));
  r.y = std::floor (area.y + pad + head + 0.5f * (ah - r.h));
  const int al = (int) (255.0f * alpha);
  dl->AddText (ImVec2 (r.x, r.y - head + 1), IM_COL32 (170, 180, 205, al), title);
  dl->AddImage ((ImTextureID) (intptr_t) tex, ImVec2 (r.x, r.y),
                ImVec2 (r.x + r.w, r.y + r.h), ImVec2 (0, 0), ImVec2 (1, 1),
                IM_COL32 (255, 255, 255, al));
  dl->AddRect (ImVec2 (r.x - 1, r.y - 1), ImVec2 (r.x + r.w + 1, r.y + r.h + 1),
               IM_COL32 (50, 56, 80, al));
  return r;
}

// The wordmark, drawn small with ImGui (used as the panel header).
static void draw_logo_small (const LogoMesh &m, ImDrawList *dl, float x, float y, float h, float alpha) {
  const float s = h / m.height;
  for (size_t t = 0; t < m.ntriangles (); t++) {
    ImVec2 p[3];
    for (int k = 0; k < 3; k++) {
      const uint32_t v = m.tri[3 * t + (size_t) k];
      p[k] = ImVec2 (x + m.vx[v] * s, y + (m.height - m.vy[v]) * s);
    }
    dl->AddTriangleFilled (p[0], p[1], p[2],
                           ImGui::ColorConvertFloat4ToU32 (ImVec4 (m.rgb[3 * t], m.rgb[3 * t + 1], m.rgb[3 * t + 2], alpha)));
  }
}

static void describe_variable (App &a, int v) {
  const uint32_t w = SG_LOAD (&a.shared.state[(size_t) v]);
  const uint32_t val = w & SG_VAL_MASK;
  const uint32_t kind = (w >> SG_KIND_SHIFT) & SG_KIND_MASK;
  static const char *kinds[] = {"propagated", "decision", "inprocessing", "model"};
  ImGui::BeginTooltip ();
  ImGui::Text ("variable %d", v);
  if (!val)
    ImGui::TextUnformatted (w & SG_GONE ? "removed by preprocessing" : "unassigned");
  else
    ImGui::Text ("%s  (%s, level %u)", val == SG_VAL_TRUE ? "true" : "false",
                 kinds[kind & 3], w >> SG_LEVEL_SHIFT);
  ImGui::Text ("assigned %u times", SG_LOAD (&a.shared.flips[(size_t) v]));
  ImGui::Text ("in %u learned clauses", SG_LOAD (&a.shared.learnt[(size_t) v]));
  ImGui::EndTooltip ();
}

static void describe_clause (App &a, size_t i) {
  ImGui::BeginTooltip ();
  ImGui::Text ("clause %zu  (%d literals)", i + 1,
               (int) (a.cnf.end (i) - a.cnf.begin (i)));
  std::string s;
  int shown = 0;
  for (const int *p = a.cnf.begin (i); p != a.cnf.end (i) && shown < 12; p++, shown++) {
    const uint32_t val = SG_LOAD (&a.shared.state[(size_t) abs (*p)]) & SG_VAL_MASK;
    char b[48];
    const char *mark = !val ? "?" : ((val == SG_VAL_TRUE) == (*p > 0)) ? "T" : "F";
    snprintf (b, sizeof b, "%d:%s  ", *p, mark);
    s += b;
  }
  if (a.cnf.end (i) - a.cnf.begin (i) > 12)
    s += "...";
  ImGui::TextUnformatted (s.c_str ());
  ImGui::EndTooltip ();
}

/*------------------------------------------------------------------------*/

static void draw_panel (App &a, Rect r, float dt) {
  const sg_ctx &c = a.shared.ctx;
  const int st = a.shared.status.load ();
  ImGui::SetNextWindowPos (ImVec2 (r.x, r.y));
  ImGui::SetNextWindowSize (ImVec2 (r.w, r.h));
  ImGui::Begin ("##panel", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus);
  // TAB must not move keyboard focus into a slider: that would turn it into
  // a text field and swallow all the shortcut keys.
  ImGui::PushItemFlag (ImGuiItemFlags_NoTabStop, true);

  {
    const float lh = 26.0f * a.opt.ui_scale;
    const ImVec2 p = ImGui::GetCursorScreenPos ();
    draw_logo_small (a.intro.mesh (), ImGui::GetWindowDrawList (), p.x + 2, p.y + 3, lh, ImGui::GetStyle ().Alpha);
    ImGui::Dummy (ImVec2 (r.w - 30, lh + 10));
  }
  ImGui::TextColored (ImVec4 (1.0f, 0.75f, 0.3f, 1), "%s", a.cnf.name.c_str ());
  ImGui::Text ("%d variables, %zu clauses, %zu graph edges", a.cnf.nvars,
               a.cnf.nclauses (), a.graph.nedges ());
  {
    int sel = a.solver;
    ImGui::SetNextItemWidth (r.w * 0.34f);
    if (ImGui::Combo ("##solver", &sel, "CaDiCaL\0Kissat\0MiniSat\0Replay\0") && sel != a.solver &&
        (sel != REPLAY_SOLVER || !replay_path ().empty ())) {
      a.solver = sel;
      restart_solver (a); // same formula, other solver
    }
    if (ImGui::IsItemHovered ())
      ImGui::SetTooltip ("%s\nChoosing a solver restarts the search (keys 1, 2, 3).", a.runner.solver_name.c_str ());
  }
  ImGui::SameLine ();
  ImVec4 sc = st == ST_SAT     ? ImVec4 (0.3f, 1.0f, 0.45f, 1)
              : st == ST_UNSAT ? ImVec4 (1.0f, 0.4f, 0.35f, 1)
              : a.paused       ? ImVec4 (1.0f, 0.9f, 0.3f, 1)
                               : ImVec4 (0.5f, 0.8f, 1.0f, 1);
  ImGui::TextColored (sc, "  %s", status_text (st, a.paused));
  if (st == ST_SAT && !a.shared.model_ok.load ())
    ImGui::TextColored (ImVec4 (1, 0.2f, 0.2f, 1), "model does NOT satisfy the formula");
  if (st >= ST_ABORTED && a.shared.hook_errors.load ())
    ImGui::TextColored (ImVec4 (1, 0.2f, 0.2f, 1), "hook self-check failed");

  // --- controls
  ImGui::Separator ();
  if (ImGui::Button (a.paused ? "Resume" : "Pause "))
    a.paused = !a.paused;
  ImGui::SameLine ();
  if (ImGui::Button ("Step"))
    a.paused = true, a.shared.tokens.fetch_add (1);
  ImGui::SameLine ();
  if (ImGui::Button ("Step x100"))
    a.paused = true, a.shared.tokens.fetch_add (100);
  ImGui::SameLine ();
  if (ImGui::Button ("Restart"))
    restart_solver (a);
  float rate = (float) a.rate;
  ImGui::SetNextItemWidth (r.w * 0.52f);
  ImGui::BeginDisabled (a.unlimited);
  if (ImGui::SliderFloat ("events/s", &rate, 1.0f, 1.0e7f, "%.0f",
                          ImGuiSliderFlags_Logarithmic))
    a.rate = rate;
  ImGui::EndDisabled ();
  ImGui::SameLine ();
  ImGui::Checkbox ("max", &a.unlimited);

  // --- search state
  ImGui::Separator ();
  const int64_t trail = SG_LOAD (&c.trail);
  const float frac = a.cnf.nvars ? (float) trail / (float) a.cnf.nvars : 0.0f;
  char buf[96];
  snprintf (buf, sizeof buf, "%lld / %d assigned", (long long) trail, a.cnf.nvars);
  ImGui::ProgressBar (frac, ImVec2 (-1, 0), buf);
  ImGui::Text ("decision level   %d   (deepest %d)", SG_LOAD (&c.level),
               SG_LOAD (&c.max_level));
  ImGui::Text ("fixed at level 0 %lld   eliminated %lld", (long long) SG_LOAD (&c.fixed),
               (long long) SG_LOAD (&c.gone));
  ImGui::Text ("decisions        %s", human ((double) SG_LOAD (&c.decisions)).c_str ());
  ImGui::Text ("propagations     %s", human ((double) SG_LOAD (&c.propagations)).c_str ());
  ImGui::Text ("inprocessing     %s", human ((double) SG_LOAD (&c.inprocessing)).c_str ());
  ImGui::Text ("unassignments    %s", human ((double) SG_LOAD (&c.unassigns)).c_str ());
  const uint64_t conflicts = SG_LOAD (&c.conflicts);
  const uint64_t backjumps = SG_LOAD (&c.backjumps);
  ImGui::Text ("conflicts        %s", human ((double) conflicts).c_str ());
  ImGui::Text ("backjumps        %s   (avg %.2f levels)", human ((double) backjumps).c_str (),
               backjumps ? (double) SG_LOAD (&c.backjump_dist) / (double) backjumps : 0.0);
  ImGui::Text ("last backjump    %d -> %d", SG_LOAD (&c.last_jump_from),
               SG_LOAD (&c.last_jump_to));
  const uint64_t learned = SG_LOAD (&c.learned);
  ImGui::Text ("learned clauses  %s   (avg size %.1f, glue %.1f)",
               human ((double) learned).c_str (),
               learned ? (double) SG_LOAD (&c.learned_lits) / (double) learned : 0.0,
               learned ? (double) SG_LOAD (&c.learned_glue) / (double) learned : 0.0);
  ImGui::Text ("restarts         %s   reductions %s",
               human ((double) SG_LOAD (&c.restarts)).c_str (),
               human ((double) SG_LOAD (&c.reductions)).c_str ());
  ImGui::Text ("speed            %s events/s, %s conflicts/s",
               human (a.events_per_s).c_str (), human (a.conflicts_per_s).c_str ());
  if (st >= ST_ABORTED)
    ImGui::Text ("solve time       %.2f s (wall clock, throttled)",
                 a.shared.solve_seconds.load ());
  else
    ImGui::Text ("running for      %.1f s", a.run_clock);

  // --- plots
  ImGui::Separator ();
  const ImVec2 ps (r.w - 24, 46);
  snprintf (buf, sizeof buf, "assigned %.0f%%", 100.0f * frac);
  ImGui::PlotLines ("##trail", a.h_trail, HISTORY, a.h_pos, buf, 0.0f, 1.0f, ps);
  snprintf (buf, sizeof buf, "decision level %d", SG_LOAD (&c.level));
  ImGui::PlotLines ("##level", a.h_level, HISTORY, a.h_pos, buf, 0.0f, FLT_MAX, ps);
  snprintf (buf, sizeof buf, "conflicts/s %s", human (a.conflicts_per_s).c_str ());
  ImGui::PlotLines ("##conf", a.h_conf, HISTORY, a.h_pos, buf, 0.0f, FLT_MAX, ps);
  ImGui::PlotHistogram ("##levels", a.level_hist, a.level_hist_bins, 0,
                        "variables per decision level", 0.0f, FLT_MAX, ps);

  // --- view settings
  if (ImGui::CollapsingHeader ("View", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth (r.w * 0.52f);
    ImGui::Combo ("layout", &a.view, "graph + grids\0graph only\0grids only\0");
    if (ImGui::Checkbox ("3D graph", &a.view3d))
      a.auto_fit = true;
    ImGui::SameLine ();
    ImGui::BeginDisabled (!a.view3d);
    ImGui::Checkbox ("auto-rotate", &a.auto_rotate);
    ImGui::EndDisabled ();
    bool cl = a.clustered;
    if (ImGui::Checkbox ("clusters", &cl))
      set_clustered (a, cl);
    if (ImGui::IsItemHovered ())
      ImGui::SetTooltip ("Group the variables into communities (Louvain) and\n"
                         "lay out only the communities. Zoom in to see the\n"
                         "variables inside a community (key K).");
    if (a.clustered) {
      ImGui::SameLine ();
      ImGui::TextDisabled ("%d, Q = %.2f", a.clu.k, a.clu.modularity);
      ImGui::SetNextItemWidth (r.w * 0.52f);
      ImGui::SliderFloat ("cluster size", &a.cluster_spread, 0.2f, 3.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SetNextItemWidth (r.w * 0.52f);
    ImGui::Combo ("colours", &a.colour_mode, "truth value\0decision level\0activity\0");
    ImGui::SetNextItemWidth (r.w * 0.52f);
    ImGui::SliderFloat ("node size", &a.node_size, 0.5f, 30.0f, "%.1f",
                        ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth (r.w * 0.52f);
    ImGui::SliderFloat ("glow", &a.glow, 0.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth (r.w * 0.52f);
    ImGui::SliderFloat ("brightness", &a.brightness, 0.05f, 2.0f, "%.2f",
                        ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox ("bloom", &a.bloom_on);
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (r.w * 0.36f);
    ImGui::BeginDisabled (!a.bloom_on);
    ImGui::SliderFloat ("##bloom", &a.bloom_strength, 0.05f, 3.0f, "strength %.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled ();
    ImGui::Checkbox ("shockwaves", &a.waves_on);
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (r.w * 0.36f);
    ImGui::BeginDisabled (!a.waves_on);
    ImGui::SliderFloat ("##waves", &a.wave_gain, 0.1f, 3.0f, "strength %.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled ();
    ImGui::Checkbox ("edges", &a.show_edges);
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (r.w * 0.36f);
    ImGui::SliderFloat ("##edgealpha", &a.edge_alpha, 0.005f, 1.0f, "alpha %.3f",
                        ImGuiSliderFlags_Logarithmic);
    bool run = a.layout.running.load ();
    if (ImGui::Checkbox ("force layout running", &run))
      a.layout.running.store (run);
    float sc2 = a.layout.scaling.load (), gr = a.layout.gravity.load ();
    ImGui::SetNextItemWidth (r.w * 0.52f);
    if (ImGui::SliderFloat ("repulsion", &sc2, 0.2f, 50.0f, "%.1f",
                            ImGuiSliderFlags_Logarithmic))
      a.layout.scaling.store (sc2);
    ImGui::SetNextItemWidth (r.w * 0.52f);
    if (ImGui::SliderFloat ("gravity", &gr, 0.0f, 20.0f, "%.2f"))
      a.layout.gravity.store (gr);
    if (ImGui::Button ("Fit view"))
      a.auto_fit = true;
    ImGui::SameLine ();
    if (ImGui::Button ("Reshuffle layout"))
      a.layout.reshuffle (), a.auto_fit = true;
    ImGui::Text ("layout: %llu iterations, %.1f ms each",
                 (unsigned long long) a.layout.iterations.load (),
                 a.layout.ms_per_iteration.load ());
    ImGui::Text ("%.0f frames/s, state update %.2f ms", dt > 0 ? 1.0 / dt : 0.0, a.update_ms);
  }

  if (ImGui::CollapsingHeader ("Capture")) {
    if (ImGui::Button ("Screenshot"))
      a.want_shot = true; // taken at the end of the frame
    ImGui::SameLine ();
    if (!a.rec.active ()) {
      if (ImGui::Button ("Record"))
        a.want_rec_toggle = true; // started at the end of the frame
      ImGui::SameLine ();
      ImGui::SetNextItemWidth (r.w * 0.30f);
      ImGui::Combo ("##fmt", &a.rec_format, "MP4\0GIF\0WebM\0");
    } else {
      if (ImGui::Button ("Stop recording"))
        a.want_rec_toggle = true;
      ImGui::SameLine ();
      ImGui::TextColored (ImVec4 (1, 0.3f, 0.3f, 1), "REC %.0f s", (double) a.rec.frames () / a.rec.fps ());
    }
    ImGui::TextDisabled ("Files go to the current directory.");
  }

  if (ImGui::CollapsingHeader ("Legend", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled ("variables");
    legend_item ("true", COL_TRUE);
    ImGui::SameLine (r.w * 0.5f);
    legend_item ("false", COL_FALSE);
    legend_item ("decision", mix (COL_TRUE, COL_WHITE, 0.55f));
    ImGui::SameLine (r.w * 0.5f);
    legend_item ("fixed (level 0)", scale (mix (COL_FALSE, COL_WHITE, 0.35f), 0.5f));
    legend_item ("just unassigned", add (COL_UNASSIGNED, scale (COL_AFTERGLOW, 0.9f)));
    ImGui::SameLine (r.w * 0.5f);
    legend_item ("in learned clause", COL_CONFLICT);
    legend_item ("eliminated", scale (COL_GONE, 2.0f));
    ImGui::TextDisabled ("clauses");
    legend_item ("satisfied", CL_SAT);
    ImGui::SameLine (r.w * 0.5f);
    legend_item ("open", mix (CL_OPEN, CL_TIGHT, 0.4f));
    legend_item ("unit", CL_UNIT);
    ImGui::SameLine (r.w * 0.5f);
    legend_item ("falsified", CL_FALSIFIED);
    legend_item ("removed by solver", scale (CL_REMOVED, 2.0f));
  }

  if (ImGui::CollapsingHeader ("Keys")) {
    ImGui::TextUnformatted (
        "SPACE  pause / resume\n"
        "S      single step (while paused)\n"
        "+ / -  double / halve the speed\n"
        "M      full speed on / off\n"
        "R      restart the solver\n"
        "1 2 3  switch to CaDiCaL / Kissat / MiniSat\n"
        "D      graph in 3D / 2D\n"
        "K      clusters on / off\n"
        "B      bloom on / off\n"
        "W      conflict shockwaves on / off\n"
        "O      start / stop recording a video\n"
        "V      cycle graph / grid layout\n"
        "C      cycle colour mode\n"
        "E      edges on / off\n"
        "L      freeze / resume force layout\n"
        "F      fit graph to window\n"
        "P      save screenshot (.ppm)\n"
        "mouse  2D: drag to pan, wheel to zoom\n"
        "       3D: drag to rotate, right-drag to pan\n"
        "ESC    quit");
  }
  if (!a.msg.empty () && wall_seconds () < a.msg_until) {
    ImGui::Separator ();
    ImGui::TextColored (ImVec4 (0.6f, 0.9f, 1.0f, 1), "%s", a.msg.c_str ());
  }
  ImGui::PopItemFlag ();
  ImGui::End ();
}

/*------------------------------------------------------------------------*/

int main (int argc, char **argv) {
  signal (SIGPIPE, SIG_IGN); // a dying ffmpeg must not take SATGlow with it
  App *app = new App ();
  App &a = *app;
  if (!parse_args (argc, argv, a.opt))
    return 2;
  if (!a.opt.logo_svg.empty ()) {
    LogoMesh logo;
    build_logo (logo);
    if (!write_logo_svg (logo, a.opt.logo_svg)) {
      fprintf (stderr, "satglow: cannot write '%s'\n", a.opt.logo_svg.c_str ());
      return 1;
    }
    printf ("satglow: wrote %s\n", a.opt.logo_svg.c_str ());
    return 0;
  }
  a.solver = a.opt.solver;
  if (!a.opt.replay.empty ()) {
    set_replay_path (a.opt.replay);
    a.solver = REPLAY_SOLVER;
  }
  a.view3d = a.opt.three_d;
  a.depth_mix = a.view3d ? 1.0f : 0.0f;
  a.bloom_on = a.opt.bloom;
  a.waves_on = a.opt.waves;

  std::string err;
  if (!parse_dimacs (a.opt.file, a.cnf, err)) {
    fprintf (stderr, "satglow: %s\n", err.c_str ());
    return 2;
  }
  printf ("satglow: %s: %d variables, %zu clauses\n", a.cnf.name.c_str (),
          a.cnf.nvars, a.cnf.nclauses ());
  build_vig (a.cnf, a.graph);
  printf ("satglow: variable interaction graph has %zu edges\n", a.graph.nedges ());

  const size_t n = (size_t) a.cnf.nvars;
  a.grid_w = std::max (1, (int) std::ceil (std::sqrt ((double) n)));
  a.grid_h = (int) ((n + (size_t) a.grid_w - 1) / (size_t) a.grid_w);
  const size_t m = std::max ((size_t) 1, a.cnf.nclauses ());
  a.cgrid_w = std::max (1, (int) std::ceil (std::sqrt ((double) m)));
  a.cgrid_h = (int) ((m + (size_t) a.cgrid_w - 1) / (size_t) a.cgrid_w);
  a.col.assign ((size_t) a.grid_w * a.grid_h * 4, 0);
  a.clause_col.assign ((size_t) a.cgrid_w * a.cgrid_h * 4, 0);
  a.size.assign (n, 1.0f);
  a.heat.assign (n, 0.0f);
  a.cheat.assign (n, 0.0f);
  a.prev_flips.assign (n, 0);
  a.prev_learnt.assign (n, 0);

  // Sensible defaults for the instance size.
  a.rate = a.opt.speed > 0 ? a.opt.speed : default_rate (a.cnf.nvars);
  a.unlimited = a.opt.unlimited;
  a.paused = a.opt.paused;
  a.edge_alpha = std::min (0.35f, std::max (0.012f, 22.0f / std::sqrt ((float) a.graph.nedges () + 1.0f)));
  a.show_edges = a.opt.edges_set ? a.opt.edges : a.graph.nedges () <= 1500000;

  // --- window
  if (!glfwInit ()) {
    fprintf (stderr, "satglow: cannot initialise GLFW (is a display available?)\n");
    return 1;
  }
  glfwWindowHint (GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint (GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint (GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint (GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  GLFWmonitor *monitor = nullptr;
  int ww = a.opt.width, wh = a.opt.height;
  if (a.opt.fullscreen) {
    monitor = glfwGetPrimaryMonitor ();
    const GLFWvidmode *mode = monitor ? glfwGetVideoMode (monitor) : nullptr;
    if (mode)
      ww = mode->width, wh = mode->height;
    else
      monitor = nullptr;
  }
  std::string title = "SATGlow - " + a.cnf.name;
  a.title = title;
  GLFWwindow *window = glfwCreateWindow (ww, wh, title.c_str (), monitor, nullptr);
  if (!window) {
    fprintf (stderr, "satglow: cannot create an OpenGL 3.3 core window\n");
    glfwTerminate ();
    return 1;
  }
  glfwMakeContextCurrent (window);
  glfwSwapInterval (1);
  if (!gladLoadGLLoader ((GLADloadproc) glfwGetProcAddress)) {
    fprintf (stderr, "satglow: cannot load OpenGL functions\n");
    return 1;
  }
  printf ("satglow: OpenGL %s on %s\n", (const char *) glGetString (GL_VERSION),
          (const char *) glGetString (GL_RENDERER));

  IMGUI_CHECKVERSION ();
  ImGui::CreateContext ();
  ImGuiIO &io = ImGui::GetIO ();
  io.IniFilename = nullptr;
  io.FontGlobalScale = a.opt.ui_scale;
  io.Fonts->AddFontDefault ();
  ImFontConfig big_cfg;
  big_cfg.SizePixels = 26.0f; // for the line under the logo in the intro
  ImFont *font_big = io.Fonts->AddFontDefault (&big_cfg);
  ImGui::StyleColorsDark ();
  ImGuiStyle &style = ImGui::GetStyle ();
  style.WindowRounding = 0.0f;
  style.WindowBorderSize = 0.0f;
  style.Colors[ImGuiCol_WindowBg] = ImVec4 (0.045f, 0.05f, 0.075f, 1.0f);
  style.Colors[ImGuiCol_PlotLines] = ImVec4 (1.0f, 0.70f, 0.25f, 1.0f);
  style.Colors[ImGuiCol_PlotHistogram] = ImVec4 (0.25f, 0.70f, 1.0f, 1.0f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4 (0.09f, 0.10f, 0.15f, 1.0f);
  ImGui_ImplGlfw_InitForOpenGL (window, true);
  ImGui_ImplOpenGL3_Init ("#version 330 core");

  {
    // Node size from how much room each node gets in the graph view.
    int w0, h0;
    glfwGetWindowSize (window, &w0, &h0);
    const double area = 0.35 * (double) w0 * (double) h0;
    a.node_size = (float) std::min (9.0, std::max (1.0, 0.33 * std::sqrt (area / (double) (n + 1))));
    a.glow = n <= 2000 ? 0.6f : n <= 20000 ? 0.4f : 0.25f;
  }
  init_gl (a);
  a.intro.init ();
  a.bloom.init ();
  a.waves.init (a.graph);
  a.bloom_strength = n <= 20000 ? 0.9f : 0.6f;
  a.shared.reset (a.cnf.nvars);
  if (a.opt.clusters == 1 || (a.opt.clusters < 0 && n > 20000))
    set_clustered (a, true);
  else
    a.layout.start (&a.graph, 12345u, a.view3d);
  if (!a.opt.record.empty ()) {
    int w0, h0;
    glfwGetFramebufferSize (window, &w0, &h0);
    std::string rec_err;
    if (!a.rec.start (a.opt.record, w0, h0, a.opt.fps, rec_err)) {
      fprintf (stderr, "satglow: %s\n", rec_err.c_str ());
      return 1;
    }
    a.offline = true;
    printf ("satglow: recording %s at %d fps%s\n", a.opt.record.c_str (), a.opt.fps,
            a.opt.record_seconds > 0 ? "" : " until the window is closed");
  }
  if (a.opt.intro)
    a.intro_t = 0; // the solver starts when the logo is complete
  else {
    a.intro_t = Intro::END;
    restart_solver (a);
  }

  auto t_prev = std::chrono::steady_clock::now ();
  double t_total = 0;

  while (!glfwWindowShouldClose (window)) {
    glfwPollEvents ();
    auto t_now = std::chrono::steady_clock::now ();
    double dt_real = std::chrono::duration<double> (t_now - t_prev).count ();
    t_prev = t_now;
    if (dt_real < 0.004) // no vsync (e.g. software rendering): do not spin
      std::this_thread::sleep_for (
          std::chrono::microseconds ((long) ((0.004 - dt_real) * 1e6)));
    const double dt_wall = dt_real;
    if (a.offline) // recording from the command line: video time, not wall time
      dt_real = 1.0 / a.opt.fps;
    const float dt = (float) std::min (dt_real, 0.1);
    t_total += dt_real;
    a.now += dt;

    int fbw, fbh, winw, winh;
    glfwGetFramebufferSize (window, &fbw, &fbh);
    glfwGetWindowSize (window, &winw, &winh);
    if (fbw <= 0 || fbh <= 0 || winw <= 0 || winh <= 0) {
      std::this_thread::sleep_for (std::chrono::milliseconds (20));
      continue;
    }
    const float fb_scale = (float) fbw / (float) winw;

    ImGui_ImplOpenGL3_NewFrame ();
    ImGui_ImplGlfw_NewFrame ();
    ImGui::NewFrame ();

    // --- intro: any key or click skips to the fade-out
    const bool intro_on = a.intro_t < Intro::END;
    if (intro_on) {
      a.intro_t += (float) std::min (dt_real, 0.25);
      if (a.intro_t < Intro::FADE_START)
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
          if (k != ImGuiKey_Escape && ImGui::IsKeyPressed ((ImGuiKey) k, false)) {
            a.intro_t = Intro::FADE_START;
            break;
          }
      if (a.intro_t >= Intro::FADE_START && !a.solver_started)
        restart_solver (a);
    }
    const bool intro_blocks = a.intro_t < Intro::FADE_START;
    const float ui_alpha = intro_on ? 1.0f - Intro::cover (a.intro_t) : 1.0f;

    // --- keyboard
    if (ImGui::IsKeyPressed (ImGuiKey_Escape) && !io.WantTextInput)
      glfwSetWindowShouldClose (window, 1);
    if (!io.WantTextInput && !intro_blocks) {
      if (ImGui::IsKeyPressed (ImGuiKey_Space, false))
        a.paused = !a.paused;
      if (ImGui::IsKeyPressed (ImGuiKey_S))
        a.paused = true, a.shared.tokens.fetch_add (1);
      if (ImGui::IsKeyPressed (ImGuiKey_Equal) || ImGui::IsKeyPressed (ImGuiKey_KeypadAdd))
        a.rate = std::min (1e7, a.rate * 2);
      if (ImGui::IsKeyPressed (ImGuiKey_Minus) || ImGui::IsKeyPressed (ImGuiKey_KeypadSubtract))
        a.rate = std::max (1.0, a.rate / 2);
      if (ImGui::IsKeyPressed (ImGuiKey_M, false))
        a.unlimited = !a.unlimited;
      if (ImGui::IsKeyPressed (ImGuiKey_R, false))
        restart_solver (a);
      if (ImGui::IsKeyPressed (ImGuiKey_V, false) || ImGui::IsKeyPressed (ImGuiKey_Tab, false))
        a.view = (a.view + 1) % 3;
      if (ImGui::IsKeyPressed (ImGuiKey_C, false))
        a.colour_mode = (a.colour_mode + 1) % 3;
      if (ImGui::IsKeyPressed (ImGuiKey_E, false))
        a.show_edges = !a.show_edges;
      if (ImGui::IsKeyPressed (ImGuiKey_L, false))
        a.layout.running.store (!a.layout.running.load ());
      if (ImGui::IsKeyPressed (ImGuiKey_F, false))
        a.auto_fit = true;
      if (ImGui::IsKeyPressed (ImGuiKey_D, false))
        a.view3d = !a.view3d, a.auto_fit = true;
      if (ImGui::IsKeyPressed (ImGuiKey_K, false))
        set_clustered (a, !a.clustered);
      if (ImGui::IsKeyPressed (ImGuiKey_B, false))
        a.bloom_on = !a.bloom_on;
      if (ImGui::IsKeyPressed (ImGuiKey_W, false))
        a.waves_on = !a.waves_on;
      if (ImGui::IsKeyPressed (ImGuiKey_O, false))
        a.want_rec_toggle = true;
      for (int k = 0; k < NUM_SOLVERS; k++)
        if (ImGui::IsKeyPressed ((ImGuiKey) (ImGuiKey_1 + k), false) && a.solver != k) {
          a.solver = k;
          restart_solver (a);
        }
    }

    // --- feed the throttle
    a.shared.unlimited.store (a.unlimited);
    a.shared.paused.store (a.paused);
    if (!a.paused && !a.unlimited) {
      a.token_acc += a.rate * dt;
      const int64_t whole = (int64_t) a.token_acc;
      a.token_acc -= (double) whole;
      if (whole > 0) {
        // Never let more than a quarter second of work pile up.
        const int64_t cap = std::max<int64_t> (1, (int64_t) (a.rate * 0.25));
        int64_t cur = a.shared.tokens.load ();
        if (cur < cap)
          a.shared.tokens.fetch_add (std::min (whole, cap - cur));
      }
    }

    // --- pull state from the other threads
    a.layout.three_d.store (a.view3d);
    {
      auto u0 = std::chrono::steady_clock::now ();
      a.depth_mix += ((a.view3d ? 1.0f : 0.0f) - a.depth_mix) * (1.0f - std::exp (-dt * 3.0f));
      if (a.clustered) {
        a.layout.snapshot (a.cpos);
        expand_positions (a.clu, a.cpos, a.depth_mix, a.cluster_spread, a.pos, a.crad);
      } else
        a.layout.snapshot (a.pos);
      update_variables (a, dt);
      update_waves (a);
      update_clauses (a);
      const float ms = std::chrono::duration<float, std::milli> (
                           std::chrono::steady_clock::now () - u0).count ();
      a.update_ms += 0.1f * (ms - a.update_ms);
    }
    const int st = a.shared.status.load ();
    if (st == ST_RUNNING && !a.paused)
      a.run_clock += dt_real;

    // --- statistics history (20 samples per second)
    a.h_timer += dt_real;
    if (a.h_timer >= 0.05) {
      const sg_ctx &c = a.shared.ctx;
      const uint64_t events = SG_LOAD (&c.assigns) + SG_LOAD (&c.unassigns);
      const uint64_t conflicts = SG_LOAD (&c.conflicts);
      const double ev = events >= a.last_events ? (double) (events - a.last_events) / a.h_timer : 0;
      const double cf = conflicts >= a.last_conflicts ? (double) (conflicts - a.last_conflicts) / a.h_timer : 0;
      a.events_per_s += 0.3 * (ev - a.events_per_s);
      a.conflicts_per_s += 0.3 * (cf - a.conflicts_per_s);
      a.last_events = events;
      a.last_conflicts = conflicts;
      a.h_trail[a.h_pos] = a.cnf.nvars ? (float) SG_LOAD (&c.trail) / (float) a.cnf.nvars : 0;
      a.h_level[a.h_pos] = (float) SG_LOAD (&c.level);
      a.h_conf[a.h_pos] = (float) cf;
      a.h_events[a.h_pos] = (float) ev;
      a.h_pos = (a.h_pos + 1) % HISTORY;
      a.h_timer = 0;
    }

    // --- screen layout (in window coordinates)
    const float W = (float) winw, H = (float) winh;
    const float panel_w = std::min (W * 0.5f, 400.0f * a.opt.ui_scale);
    Rect panel = {W - panel_w, 0, panel_w, H};
    const float main_w = W - panel_w;
    Rect graph_r = {0, 0, main_w, H}, vars_r = {0, 0, 0, 0}, clauses_r = {0, 0, 0, 0};
    if (a.view == VIEW_SPLIT) {
      const float gw = std::floor (std::min (main_w * 0.36f, H * 0.5f + 16.0f));
      graph_r = {0, 0, main_w - gw, H};
      vars_r = {main_w - gw, 0, gw, H * 0.5f};
      clauses_r = {main_w - gw, H * 0.5f, gw, H * 0.5f};
    } else if (a.view == VIEW_GRIDS) {
      graph_r = {0, 0, 0, 0};
      vars_r = {0, 0, main_w * 0.5f, H};
      clauses_r = {main_w * 0.5f, 0, main_w * 0.5f, H};
    }

    // --- camera
    // One perspective camera serves both modes.  In 2D all nodes lie in the
    // z = 0 plane and the camera looks straight at it, which is the same as
    // a flat view; switching to 3D only lets the camera leave that axis.
    const ImVec2 mouse = io.MousePos;
    const float tan_half = std::tan (0.5f * FOVY);
    if (graph_r.w > 1 && n > 0) {
      const float aspect = graph_r.w / graph_r.h;
      float lo[3], hi[3];
      graph_bounds (a, lo, hi);
      const float hx = std::max (1e-3f, 0.5f * (hi[0] - lo[0])), hy = std::max (1e-3f, 0.5f * (hi[1] - lo[1])),
                  hz = 0.5f * (hi[2] - lo[2]);
      const float margin = 40.0f + 3.0f * a.node_size;
      const float use_w = std::max (0.2f, 1.0f - 2.0f * margin / graph_r.w);
      const float use_h = std::max (0.2f, 1.0f - 2.0f * margin / graph_r.h);
      if (a.view3d) {
        const float radius = 0.70f * std::sqrt (hx * hx + hy * hy + hz * hz);
        a.fit_dist = std::max (radius / use_h, radius / (aspect * use_w)) / tan_half;
      } else
        a.fit_dist = std::max (hy / use_h, hx / (aspect * use_w)) / tan_half;
      if (a.auto_fit) {
        const float k = a.cam_init ? 1.0f - std::exp (-dt * 5.0f) : 1.0f;
        for (int d = 0; d < 3; d++)
          a.cam_t[d] += (0.5f * (lo[d] + hi[d]) - a.cam_t[d]) * k;
        a.cam_dist += (a.fit_dist - a.cam_dist) * k;
        a.cam_init = true;
      }

      const bool inside = graph_r.contains (mouse.x, mouse.y) && !io.WantCaptureMouse && !intro_blocks;
      if (inside && ImGui::IsMouseClicked (0))
        a.dragging = 1;
      else if (inside && (ImGui::IsMouseClicked (1) || ImGui::IsMouseClicked (2)))
        a.dragging = 2;
      if (!ImGui::IsMouseDown (0) && !ImGui::IsMouseDown (1) && !ImGui::IsMouseDown (2))
        a.dragging = 0;

      // Angles: free in 3D, eased back to the front view in 2D.
      if (a.view3d) {
        if (a.dragging == 1) {
          a.yaw -= io.MouseDelta.x * 0.006f;
          a.pitch = std::min (1.5f, std::max (-1.5f, a.pitch + io.MouseDelta.y * 0.006f));
          a.pitch_goal = a.pitch;
        } else {
          if (a.auto_rotate)
            a.yaw += dt * 0.22f;
          a.pitch += (a.pitch_goal - a.pitch) * (1.0f - std::exp (-dt * 3.0f));
        }
      } else {
        a.yaw = std::remainder (a.yaw, 6.2831853f);
        const float k = 1.0f - std::exp (-dt * 6.0f);
        a.yaw -= a.yaw * k;
        a.pitch -= a.pitch * k;
        if (std::fabs (a.yaw) < 1e-4f && std::fabs (a.pitch) < 1e-4f)
          a.yaw = a.pitch = 0;
      }
      const float cp = std::cos (a.pitch), sp = std::sin (a.pitch), cy = std::cos (a.yaw), sy = std::sin (a.yaw);
      const float back[3] = {cp * sy, sp, cp * cy}; // from the target towards the camera
      const float right[3] = {cy, 0, -sy};
      const float up[3] = {-sp * sy, cp, -sp * cy};

      const float ndc_x = (mouse.x - (graph_r.x + 0.5f * graph_r.w)) / (0.5f * graph_r.w);
      const float ndc_y = -(mouse.y - (graph_r.y + 0.5f * graph_r.h)) / (0.5f * graph_r.h);
      if (inside && io.MouseWheel != 0.0f) {
        const float d = a.cam_dist / std::pow (1.15f, io.MouseWheel);
        if (!a.view3d) {
          // Zoom around the cursor: keep the point under it where it is.
          const float shift = (a.cam_dist - d) * tan_half;
          for (int k = 0; k < 3; k++)
            a.cam_t[k] += shift * (ndc_x * aspect * right[k] + ndc_y * up[k]);
        }
        a.cam_dist = d;
        a.auto_fit = false;
      }
      if (a.dragging && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && (a.dragging == 2 || !a.view3d)) {
        const float world_per_px = 2.0f * a.cam_dist * tan_half / graph_r.h;
        for (int k = 0; k < 3; k++)
          a.cam_t[k] += world_per_px * (-io.MouseDelta.x * right[k] + io.MouseDelta.y * up[k]);
        a.auto_fit = false;
      }

      const float eye[3] = {a.cam_t[0] + a.cam_dist * back[0], a.cam_t[1] + a.cam_dist * back[1],
                            a.cam_t[2] + a.cam_dist * back[2]};
      a.mvp = mat_mul (mat_perspective (FOVY, aspect, 0.02f * a.cam_dist, 80.0f * a.cam_dist),
                       mat_view (eye, right, up, back));
    }

    // --- draw the graph
    glViewport (0, 0, fbw, fbh);
    glDisable (GL_SCISSOR_TEST);
    glClearColor (0.012f, 0.014f, 0.026f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    glBindBuffer (GL_ARRAY_BUFFER, a.vbo_col);
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (n * 4), a.col.data ());
    if (graph_r.w > 1) {
      glBindBuffer (GL_ARRAY_BUFFER, a.vbo_pos);
      glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (n * 3 * sizeof (float)), a.pos.data ());
      glBindBuffer (GL_ARRAY_BUFFER, a.vbo_size);
      glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (n * sizeof (float)), a.size.data ());

      // In a cluster many nodes share a few pixels and their light adds up.
      // Dim each node by how crowded its cluster is at the current zoom, so
      // that zoomed out a cluster glows instead of burning white and zoomed
      // in its variables come back to full brightness.
      if (a.clustered || a.gain[0] != 1.0f) {
        const float px_per_unit = graph_r.h * fb_scale / (2.0f * a.cam_dist * tan_half);
        const float core = std::max (1.0f, 2.5f * a.node_size * fb_scale); // core plus halo
        std::vector<float> cgain ((size_t) std::max (a.clu.k, 1), 1.0f);
        if (a.clustered)
          for (int c = 0; c < a.clu.k; c++) {
            const float r = std::max (1.0f, a.crad[(size_t) c] * px_per_unit);
            const float crowd = (float) a.clu.size[(size_t) c] * core * core / (3.14159f * r * r);
            cgain[(size_t) c] = std::max (0.04f, std::min (1.0f, 1.7f / std::sqrt (std::max (crowd, 1e-6f))));
          }
        for (size_t v = 0; v < n; v++)
          a.gain[v] = a.clustered ? cgain[(size_t) a.clu.of[v]] : 1.0f;
        glBindBuffer (GL_ARRAY_BUFFER, a.vbo_gain);
        glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (n * sizeof (float)), a.gain.data ());
      }

      // The graph goes into a floating-point buffer first, for the bloom.
      const GLint vx = (GLint) (graph_r.x * fb_scale), vy = (GLint) ((H - graph_r.y - graph_r.h) * fb_scale);
      const GLsizei vw = (GLsizei) (graph_r.w * fb_scale), vh = (GLsizei) (graph_r.h * fb_scale);
      a.bloom.begin (vw, vh, 0.012f, 0.014f, 0.026f);
      glEnable (GL_BLEND);
      glBlendFunc (GL_ONE, GL_ONE); // additive: overlapping lights add up
      glDisable (GL_DEPTH_TEST);
      glEnable (GL_PROGRAM_POINT_SIZE);

      if (a.show_edges) {
        glUseProgram (a.prog_edge);
        glUniformMatrix4fv (glGetUniformLocation (a.prog_edge, "uMVP"), 1, GL_FALSE, a.mvp.m);
        glUniform1f (glGetUniformLocation (a.prog_edge, "uRefW"), a.cam_dist);
        glUniform1f (glGetUniformLocation (a.prog_edge, "uAlpha"), a.edge_alpha);
        if (a.clustered && a.clu.cgraph.nedges ()) {
          // Edges between clusters, coloured like the clusters' members.
          const size_t k = (size_t) a.clu.k;
          a.csum.assign (k * 3, 0.0f);
          for (size_t v = 0; v < n; v++) {
            const size_t c = (size_t) a.clu.of[v];
            for (int d = 0; d < 3; d++)
              a.csum[3 * c + (size_t) d] += a.col[4 * v + (size_t) d];
          }
          for (size_t c = 0; c < k; c++)
            for (int d = 0; d < 3; d++)
              a.ccol[4 * c + (size_t) d] = (uint8_t) std::min (255.0f, 1.6f * a.csum[3 * c + (size_t) d] / (float) a.clu.size[c]);
          glBindVertexArray (a.cvao);
          glBindBuffer (GL_ARRAY_BUFFER, a.cvbo_pos);
          glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (k * 3 * sizeof (float)), a.cpos.data ());
          glBindBuffer (GL_ARRAY_BUFFER, a.cvbo_col);
          glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) (k * 4), a.ccol.data ());
          glDrawElements (GL_LINES, (GLsizei) a.clu.cgraph.edges.size (), GL_UNSIGNED_INT, nullptr);
        } else if (!a.clustered && a.graph.nedges ()) {
          glBindVertexArray (a.vao);
          glDrawElements (GL_LINES, (GLsizei) a.graph.edges.size (), GL_UNSIGNED_INT, nullptr);
        }
      }

      // Nodes grow a little when the user zooms in beyond the fitted view.
      const float zoom_boost = std::sqrt (std::max (1.0f, a.fit_dist / a.cam_dist));
      glBindVertexArray (a.vao);
      glUseProgram (a.prog_node);
      glUniformMatrix4fv (glGetUniformLocation (a.prog_node, "uMVP"), 1, GL_FALSE, a.mvp.m);
      glUniform1f (glGetUniformLocation (a.prog_node, "uRefW"), a.cam_dist);
      glUniform1f (glGetUniformLocation (a.prog_node, "uPointScale"), a.node_size * zoom_boost * fb_scale);
      glUniform1f (glGetUniformLocation (a.prog_node, "uMaxPoint"), a.max_point);
      glUniform1f (glGetUniformLocation (a.prog_node, "uGlow"), a.glow);
      glUniform1f (glGetUniformLocation (a.prog_node, "uBright"), a.brightness);
      glDrawArrays (GL_POINTS, 0, (GLsizei) n);
      glBindVertexArray (0);
      glUseProgram (0);

      if (a.waves_on)
        a.waves.draw_rings (a.pos, a.mvp.m, vw, vh, a.now, a.wave_gain * a.brightness);

      a.bloom.finish (vx, vy, vw, vh, a.bloom_on, a.bloom_strength, 0.55f);
      glDisable (GL_BLEND);
      glViewport (0, 0, fbw, fbh);
    }

    // --- pixel grids
    glBindTexture (GL_TEXTURE_2D, a.tex_vars);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, a.grid_w, a.grid_h, GL_RGBA, GL_UNSIGNED_BYTE, a.col.data ());
    glBindTexture (GL_TEXTURE_2D, a.tex_clauses);
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, a.cgrid_w, a.cgrid_h, GL_RGBA, GL_UNSIGNED_BYTE, a.clause_col.data ());
    glBindTexture (GL_TEXTURE_2D, 0);

    int hover_var = 0;
    long hover_clause = -1;
    if (vars_r.w > 1 && ui_alpha > 0.004f) {
      char t1[64], t2[64];
      snprintf (t1, sizeof t1, "variables (%d)", a.cnf.nvars);
      snprintf (t2, sizeof t2, "clauses (%zu)", a.cnf.nclauses ());
      Rect gv = draw_grid (a.tex_vars, a.grid_w, a.grid_h, vars_r, t1, ui_alpha);
      Rect gc = draw_grid (a.tex_clauses, a.cgrid_w, a.cgrid_h, clauses_r, t2, ui_alpha);
      if (!io.WantCaptureMouse && !intro_blocks) {
        if (gv.w > 0 && gv.contains (mouse.x, mouse.y)) {
          const int cx = (int) ((mouse.x - gv.x) / gv.w * (float) a.grid_w);
          const int cy = (int) ((mouse.y - gv.y) / gv.h * (float) a.grid_h);
          const long idx = (long) cy * a.grid_w + cx;
          if (idx >= 0 && idx < (long) n)
            hover_var = (int) idx + 1;
        } else if (gc.w > 0 && gc.contains (mouse.x, mouse.y)) {
          const int cx = (int) ((mouse.x - gc.x) / gc.w * (float) a.cgrid_w);
          const int cy = (int) ((mouse.y - gc.y) / gc.h * (float) a.cgrid_h);
          const long idx = (long) cy * a.cgrid_w + cx;
          if (idx >= 0 && idx < (long) a.cnf.nclauses ())
            hover_clause = idx;
        }
      }
    }
    // Hovering over the graph picks the node nearest to the cursor on screen.
    if (!hover_var && hover_clause < 0 && graph_r.w > 1 && !io.WantCaptureMouse && !intro_blocks &&
        !a.dragging && n <= 500000 && graph_r.contains (mouse.x, mouse.y)) {
      float best = 12.0f * 12.0f;
      for (size_t i = 0; i < n; i++) {
        float px, py;
        if (!project_node (a, graph_r, i, px, py))
          continue;
        const float d2 = (px - mouse.x) * (px - mouse.x) + (py - mouse.y) * (py - mouse.y);
        if (d2 < best)
          best = d2, hover_var = (int) i + 1;
      }
    }
    if (hover_var) {
      describe_variable (a, hover_var);
      float px, py;
      if (graph_r.w > 1 && project_node (a, graph_r, (size_t) hover_var - 1, px, py) &&
          graph_r.contains (px, py)) // ring around the node in the graph
        ImGui::GetBackgroundDrawList ()->AddCircle (ImVec2 (px, py), 4.0f + a.node_size * 1.5f,
                                                    IM_COL32 (255, 255, 255, 220), 24, 1.5f);
    } else if (hover_clause >= 0)
      describe_clause (a, (size_t) hover_clause);

    if (ui_alpha > 0.004f) {
      ImGui::PushStyleVar (ImGuiStyleVar_Alpha, ui_alpha);
      draw_panel (a, panel, (float) dt_real);
      ImGui::PopStyleVar ();
    }

    // --- intro: the logo assembles on top of everything else
    if (intro_on) {
      a.intro.draw (a.intro_t, fbw, fbh);
      const float sub = std::min (1.0f, std::max (0.0f, (a.intro_t - 2.9f) / 0.6f)) * Intro::cover (a.intro_t);
      if (sub > 0.01f) {
        const char *text = "watch a SAT solver think";
        const float size = font_big->FontSize * a.opt.ui_scale;
        const ImVec2 ts = font_big->CalcTextSizeA (size, FLT_MAX, 0.0f, text);
        const float logo_h = (a.intro.logo_right - a.intro.logo_left) / a.intro.mesh ().width;
        ImGui::GetForegroundDrawList ()->AddText (
            font_big, size, ImVec2 (0.5f * (W - ts.x), (a.intro.logo_bottom + 0.45f * logo_h) / fb_scale),
            IM_COL32 (150, 172, 220, (int) (sub * 255.0f)), text);
      }
    }
    ImGui::Render ();
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData ());

    // --- screenshots
    if ((!io.WantTextInput && ImGui::IsKeyPressed (ImGuiKey_P, false)) || a.want_shot) {
      a.want_shot = false;
      char name[64];
      snprintf (name, sizeof name, "satglow-%03d.ppm", a.screenshot_counter++);
      save_screenshot (name, fbw, fbh);
      notify (a, std::string ("saved ") + name);
    }

    // --- recording
    if (a.want_rec_toggle) {
      a.want_rec_toggle = false;
      if (a.rec.active ()) {
        a.rec.stop ();
        notify (a, "saved " + a.rec.path ());
        glfwSetWindowTitle (window, a.title.c_str ());
      } else {
        static const char *ext[] = {"mp4", "gif", "webm"};
        const std::string path = next_free_name ("satglow", ext[a.rec_format]);
        std::string rec_err;
        if (a.rec.start (path, fbw, fbh, a.opt.fps, rec_err)) {
          a.rec_acc = 1.0 / a.opt.fps; // first frame right away
          notify (a, "recording " + path + " (O stops)");
        } else
          notify (a, rec_err);
      }
    }
    if (a.rec.active ()) {
      if (fbw != a.rec.width () || fbh != a.rec.height ()) {
        a.rec.stop ();
        notify (a, "window size changed, recording stopped: " + a.rec.path ());
        glfwSetWindowTitle (window, a.title.c_str ());
      } else {
        int copies = 1;
        if (!a.offline) { // keep the video in step with the wall clock
          a.rec_acc += dt_wall;
          copies = (int) (a.rec_acc * a.opt.fps);
          a.rec_acc -= (double) copies / a.opt.fps;
          copies = std::min (copies, 4);
        }
        a.rec.frame (copies);
        if (a.rec.active () && a.rec.frames () % a.opt.fps == 0) {
          const long secs = a.rec.frames () / a.opt.fps;
          char t[300];
          snprintf (t, sizeof t, "%s   [REC %ld:%02ld]", a.title.c_str (), secs / 60, secs % 60);
          glfwSetWindowTitle (window, t);
        }
        if (a.offline && a.opt.record_seconds > 0 &&
            (double) a.rec.frames () >= a.opt.record_seconds * a.opt.fps) {
          a.rec.stop ();
          glfwSetWindowShouldClose (window, 1);
        }
      }
    }
    if (!a.opt.shot.empty () && t_total >= a.opt.shot_after) {
      save_screenshot (a.opt.shot, fbw, fbh);
      glfwSetWindowShouldClose (window, 1);
    }

    glfwSwapBuffers (window);
  }

  a.rec.stop ();
  a.layout.stop ();
  a.runner.stop ();
  ImGui_ImplOpenGL3_Shutdown ();
  ImGui_ImplGlfw_Shutdown ();
  ImGui::DestroyContext ();
  glfwDestroyWindow (window);
  glfwTerminate ();
  delete app;
  return 0;
}
