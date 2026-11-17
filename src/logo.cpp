#include "logo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct P {
  float x, y;
};

const float STROKE = 0.17f; // stroke width, capital height is 1
const float HALF = 0.5f * STROKE;
const float STEP = 0.105f; // target length of one ribbon segment
const float XHEIGHT = 0.64f;

// Cheap deterministic pseudo-random number in [0, 1).
float hash01 (uint32_t i) {
  i ^= i >> 16;
  i *= 0x7feb352dU;
  i ^= i >> 15;
  i *= 0x846ca68bU;
  i ^= i >> 16;
  return (float) (i & 0xffffff) / (float) 0x1000000;
}

void add_point (std::vector<P> &out, P p) {
  if (!out.empty () && std::fabs (out.back ().x - p.x) < 1e-5f &&
      std::fabs (out.back ().y - p.y) < 1e-5f)
    return;
  out.push_back (p);
}

void add_line (std::vector<P> &out, P a, P b) {
  const float len = std::hypot (b.x - a.x, b.y - a.y);
  const int n = std::max (1, (int) std::lround (len / STEP));
  for (int i = 0; i <= n; i++) {
    const float t = (float) i / (float) n;
    add_point (out, {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
  }
}

// Arc from angle a0 to a1 (degrees, either direction).
void add_arc (std::vector<P> &out, float cx, float cy, float r, float a0, float a1) {
  const float rad = 3.14159265f / 180.0f;
  const float len = std::fabs (a1 - a0) * rad * r;
  const int n = std::max (2, (int) std::lround (len / STEP));
  for (int i = 0; i <= n; i++) {
    const float a = (a0 + (a1 - a0) * (float) i / (float) n) * rad;
    add_point (out, {cx + r * std::cos (a), cy + r * std::sin (a)});
  }
}

struct Builder {
  LogoMesh &m;
  float ox = 0;   // x offset of the current letter
  int letter = 0; // 0..6 for S A T G l o w
  std::vector<int> tri_letter;

  uint32_t vertex (float x, float y) {
    m.vx.push_back (x + ox);
    m.vy.push_back (y);
    return (uint32_t) (m.vx.size () - 1);
  }
  void triangle (uint32_t a, uint32_t b, uint32_t c) {
    m.tri.push_back (a);
    m.tri.push_back (b);
    m.tri.push_back (c);
    tri_letter.push_back (letter);
  }

  // A ribbon of the given width along the centre line `c`.  Three vertices
  // across (edge, middle, edge) and four triangles per segment.  `flat0` /
  // `flat1` cut the first / last end horizontally instead of square.
  void ribbon (const std::vector<P> &c, float width, bool closed = false,
               bool flat0 = false, bool flat1 = false) {
    const int n = (int) c.size ();
    const float h = 0.5f * width;
    std::vector<uint32_t> L ((size_t) n), M ((size_t) n), R ((size_t) n);
    for (int i = 0; i < n; i++) {
      const bool first = i == 0 && !closed, last = i == n - 1 && !closed;
      const P prev = c[(size_t) ((i + n - 1) % n)], next = c[(size_t) ((i + 1) % n)], cur = c[(size_t) i];
      float t0x = cur.x - prev.x, t0y = cur.y - prev.y;
      float t1x = next.x - cur.x, t1y = next.y - cur.y;
      if (first)
        t0x = t1x, t0y = t1y;
      if (last)
        t1x = t0x, t1y = t0y;
      const float l0 = std::hypot (t0x, t0y), l1 = std::hypot (t1x, t1y);
      t0x /= l0, t0y /= l0, t1x /= l1, t1y /= l1;
      const float n0x = -t0y, n0y = t0x, n1x = -t1y, n1y = t1x;
      float nx = n0x + n1x, ny = n0y + n1y;
      const float nl = std::hypot (nx, ny);
      nx /= nl, ny /= nl;
      // Mitre: widen the offset in corners so the ribbon keeps its width.
      const float mitre = std::min (2.2f, 1.0f / std::max (0.3f, nx * n0x + ny * n0y));
      float px = nx * h * mitre, py = ny * h * mitre;
      if (((first && flat0) || (last && flat1)) && std::fabs (nx) > 0.3f) {
        px = h / nx; // slide along x until the end is a horizontal cut
        py = 0;
      }
      // Nudge the middle vertex so the triangles are not all alike.
      float mx = cur.x, my = cur.y;
      if (!first && !last) {
        const uint32_t seed = (uint32_t) (m.vx.size () * 2654435761u);
        const float along = (hash01 (seed) - 0.5f) * 0.55f * std::min (l0, l1);
        const float across = (hash01 (seed + 17u) - 0.5f) * 0.5f * h;
        const float tx = 0.5f * (t0x + t1x), ty = 0.5f * (t0y + t1y);
        mx += tx * along + nx * across;
        my += ty * along + ny * across;
      }
      L[(size_t) i] = vertex (cur.x + px, cur.y + py);
      M[(size_t) i] = vertex (mx, my);
      R[(size_t) i] = vertex (cur.x - px, cur.y - py);
    }
    const int segments = closed ? n : n - 1;
    for (int i = 0; i < segments; i++) {
      const size_t a = (size_t) i, b = (size_t) ((i + 1) % n);
      if (i & 1) {
        triangle (L[a], M[a], M[b]);
        triangle (L[a], M[b], L[b]);
        triangle (M[a], R[a], M[b]);
        triangle (R[a], R[b], M[b]);
      } else {
        triangle (L[a], M[a], L[b]);
        triangle (M[a], M[b], L[b]);
        triangle (M[a], R[a], R[b]);
        triangle (M[a], R[b], M[b]);
      }
    }
  }
};

} // namespace

void build_logo (LogoMesh &mesh) {
  mesh = LogoMesh ();
  Builder b{mesh, 0.0f, 0, {}};
  const float gap = 0.115f;
  std::vector<P> c;

  // S: two arcs that meet in the middle.
  {
    b.letter = 0;
    const float r = (1.0f - STROKE) / 4.0f;
    const float cx = r + HALF;
    c.clear ();
    add_arc (c, cx, 1.0f - HALF - r, r, 18.0f, 270.0f);
    add_arc (c, cx, HALF + r, r, 90.0f, -162.0f);
    b.ribbon (c, STROKE);
    b.ox += 2.0f * r + STROKE + gap;
  }
  // A: two legs with a flat top, and a crossbar.
  {
    b.letter = 1;
    const float w = 0.86f, top = 1.0f - HALF, foot = HALF * 1.05f;
    const P p0 = {foot, 0}, p1 = {0.5f * w - 0.07f, top}, p2 = {0.5f * w + 0.07f, top}, p3 = {w - foot, 0};
    c.clear ();
    add_line (c, p0, p1);
    add_line (c, p1, p2);
    add_line (c, p2, p3);
    b.ribbon (c, STROKE, false, true, true);
    const float yb = 0.29f;
    const float slope = (p1.x - p0.x) / (p1.y - p0.y);
    const float xl = p0.x + slope * yb + HALF * std::sqrt (1.0f + slope * slope) - 0.01f;
    c.clear ();
    add_line (c, {xl, yb}, {w - xl, yb});
    b.ribbon (c, STROKE * 0.86f);
    b.ox += w + gap - 0.07f; // tuck the T under the A a little
  }
  // T
  {
    b.letter = 2;
    const float w = 0.70f;
    c.clear ();
    add_line (c, {0, 1.0f - HALF}, {w, 1.0f - HALF});
    b.ribbon (c, STROKE);
    c.clear ();
    add_line (c, {0.5f * w, 1.0f - STROKE}, {0.5f * w, 0});
    b.ribbon (c, STROKE);
    b.ox += w + gap;
  }
  // G: three quarters of a circle and a bar.
  {
    b.letter = 3;
    const float r = 0.5f - HALF;
    c.clear ();
    add_arc (c, 0.5f, 0.5f, r, 44.0f, 371.0f);
    b.ribbon (c, STROKE);
    c.clear ();
    add_line (c, {0.50f, 0.5f}, {0.5f + r - HALF + 0.01f, 0.5f});
    b.ribbon (c, STROKE);
    b.ox += 1.0f + gap;
  }
  // l
  {
    b.letter = 4;
    c.clear ();
    add_line (c, {HALF, 1.04f}, {HALF, 0});
    b.ribbon (c, STROKE);
    b.ox += STROKE + gap;
  }
  // o
  {
    b.letter = 5;
    const float r = 0.5f * XHEIGHT - HALF;
    c.clear ();
    add_arc (c, 0.5f * XHEIGHT, 0.5f * XHEIGHT, r, 90.0f, 450.0f);
    c.pop_back (); // closed: the last point repeats the first
    b.ribbon (c, STROKE, true);
    b.ox += XHEIGHT + gap;
  }
  // w: a zigzag with flattened corners.
  {
    b.letter = 6;
    const float w = 0.98f, lowy = HALF, midy = 0.50f, e = 0.035f;
    const float x0 = HALF * 1.05f, x1 = 0.25f * w + 0.01f, x2 = 0.5f * w, x3 = 0.75f * w - 0.01f, x4 = w - HALF * 1.05f;
    c.clear ();
    add_line (c, {x0, XHEIGHT}, {x1 - e, lowy});
    add_line (c, {x1 - e, lowy}, {x1 + e, lowy});
    add_line (c, {x1 + e, lowy}, {x2 - e, midy});
    add_line (c, {x2 - e, midy}, {x2 + e, midy});
    add_line (c, {x2 + e, midy}, {x3 - e, lowy});
    add_line (c, {x3 - e, lowy}, {x3 + e, lowy});
    add_line (c, {x3 + e, lowy}, {x4, XHEIGHT});
    b.ribbon (c, STROKE, false, true, true);
    b.ox += w;
  }

  mesh.width = b.ox;
  mesh.height = 1.04f;

  // Colours: SAT in amber (the colour of "true"), Glow in blue ("false"),
  // every triangle a slightly different shade.
  const float split = 2.42f; // x position between the T and the G
  for (size_t t = 0; t < mesh.ntriangles (); t++) {
    float cx = 0, cy = 0;
    for (int k = 0; k < 3; k++) {
      cx += mesh.vx[mesh.tri[3 * t + (size_t) k]] / 3.0f;
      cy += mesh.vy[mesh.tri[3 * t + (size_t) k]] / 3.0f;
    }
    float r, g, bl;
    if (b.tri_letter[t] <= 2) {
      const float u = std::min (1.0f, std::max (0.0f, cx / split));
      r = 1.0f;
      g = 0.80f - 0.36f * u;
      bl = 0.24f - 0.16f * u;
    } else {
      const float u = std::min (1.0f, std::max (0.0f, (cx - split) / (mesh.width - split)));
      r = 0.26f - 0.12f * u;
      g = 0.86f - 0.40f * u;
      bl = 1.0f;
    }
    const float shade = 0.60f + 0.40f * hash01 ((uint32_t) t * 7919u + 13u) + 0.06f * cy;
    mesh.rgb.push_back (std::min (1.0f, r * shade));
    mesh.rgb.push_back (std::min (1.0f, g * shade));
    mesh.rgb.push_back (std::min (1.0f, bl * shade));
  }
}

bool write_logo_svg (const LogoMesh &m, const std::string &path) {
  FILE *f = fopen (path.c_str (), "w");
  if (!f)
    return false;
  const float pad = 0.55f, s = 200.0f;
  const float W = (m.width + 2 * pad) * s, H = (m.height + 2 * pad) * s;
  auto X = [&] (float x) { return (x + pad) * s; };
  auto Y = [&] (float y) { return (m.height - y + pad) * s; };
  fprintf (f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.0f %.0f\" width=\"%.0f\" height=\"%.0f\">\n", W, H, W, H);
  fprintf (f, "<title>SATGlow</title>\n");
  fprintf (f, "<defs>\n"
              "<filter id=\"halo\" x=\"-20%%\" y=\"-60%%\" width=\"140%%\" height=\"220%%\"><feGaussianBlur stdDeviation=\"%.1f\"/></filter>\n"
              "<filter id=\"spark\" x=\"-300%%\" y=\"-300%%\" width=\"700%%\" height=\"700%%\"><feGaussianBlur stdDeviation=\"%.1f\"/></filter>\n"
              "</defs>\n",
           0.085f * s, 0.018f * s);
  fprintf (f, "<rect width=\"%.0f\" height=\"%.0f\" rx=\"%.0f\" fill=\"#04050a\"/>\n", W, H, 0.22f * s);
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0)
      fprintf (f, "<g filter=\"url(#halo)\" opacity=\"0.75\">\n");
    else
      fprintf (f, "<g stroke-width=\"%.2f\" stroke-linejoin=\"round\">\n", 0.006f * s);
    for (size_t t = 0; t < m.ntriangles (); t++) {
      const uint32_t a = m.tri[3 * t], b = m.tri[3 * t + 1], c = m.tri[3 * t + 2];
      const int r = (int) (m.rgb[3 * t] * 255.0f + 0.5f), g = (int) (m.rgb[3 * t + 1] * 255.0f + 0.5f),
                bl = (int) (m.rgb[3 * t + 2] * 255.0f + 0.5f);
      fprintf (f, "<polygon points=\"%.1f,%.1f %.1f,%.1f %.1f,%.1f\" fill=\"#%02x%02x%02x\"", X (m.vx[a]), Y (m.vy[a]),
               X (m.vx[b]), Y (m.vy[b]), X (m.vx[c]), Y (m.vy[c]), r, g, bl);
      if (pass == 1)
        fprintf (f, " stroke=\"#%02x%02x%02x\"", std::min (255, r + 70), std::min (255, g + 70), std::min (255, bl + 70));
      fprintf (f, "/>\n");
    }
    fprintf (f, "</g>\n");
  }
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0)
      fprintf (f, "<g filter=\"url(#spark)\" fill=\"#ffffff\" opacity=\"0.65\">\n");
    else
      fprintf (f, "<g fill=\"#ffffff\" opacity=\"0.95\">\n");
    for (size_t i = 0; i < m.vx.size (); i++)
      fprintf (f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"%.2f\"/>\n", X (m.vx[i]), Y (m.vy[i]),
               (pass == 0 ? 0.022f : 0.0085f) * s);
    fprintf (f, "</g>\n");
  }
  fprintf (f, "</svg>\n");
  fclose (f);
  return true;
}
