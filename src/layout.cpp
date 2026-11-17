#include "layout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

void Layout::start (const Graph *g, unsigned seed, bool in_3d, const std::vector<float> *init) {
  stop ();
  g_ = g;
  quit_.store (false);
  three_d.store (in_3d);
  deep_ = in_3d;
  const size_t n = (size_t) g->n;
  for (std::vector<float> *v : {&x_, &y_, &z_, &dx_, &dy_, &dz_, &odx_, &ody_, &odz_})
    v->assign (n, 0.0f);
  scatter (seed);
  if (init && init->size () == 3 * n)
    for (size_t i = 0; i < n; i++) {
      x_[i] = (*init)[3 * i];
      y_[i] = (*init)[3 * i + 1];
      z_[i] = (*init)[3 * i + 2];
    }
  published_.resize (3 * n);
  for (size_t i = 0; i < n; i++) {
    published_[3 * i] = x_[i];
    published_[3 * i + 1] = y_[i];
    published_[3 * i + 2] = z_[i];
  }
  unsigned hw = std::thread::hardware_concurrency ();
  workers_ = n > 4000 ? (int) std::max (1u, std::min (6u, hw / 2)) : 1;
  thread_ = std::thread ([this, seed] { run (seed); });
}

void Layout::stop () {
  if (thread_.joinable ()) {
    quit_.store (true);
    thread_.join ();
  }
}

void Layout::snapshot (std::vector<float> &xyz) {
  std::lock_guard<std::mutex> lock (mutex_);
  xyz = published_;
}

void Layout::scatter (unsigned seed) {
  std::mt19937 rng (seed);
  std::uniform_real_distribution<float> uni (0.0f, 1.0f);
  const size_t n = x_.size ();
  const float radius = 10.0f * std::sqrt ((float) n + 1.0f);
  for (size_t i = 0; i < n; i++) {
    if (deep_) { // uniform in a ball
      const float r = radius * std::cbrt (uni (rng));
      const float cz = 2.0f * uni (rng) - 1.0f;
      const float s = std::sqrt (std::max (0.0f, 1.0f - cz * cz));
      const float a = 6.2831853f * uni (rng);
      x_[i] = r * s * std::cos (a);
      y_[i] = r * s * std::sin (a);
      z_[i] = r * cz;
    } else { // uniform in a disc
      const float r = radius * std::sqrt (uni (rng));
      const float a = 6.2831853f * uni (rng);
      x_[i] = r * std::cos (a);
      y_[i] = r * std::sin (a);
      z_[i] = 0;
    }
    dx_[i] = dy_[i] = dz_[i] = odx_[i] = ody_[i] = odz_[i] = 0;
  }
  speed_ = 1.0;
  speed_efficiency_ = 1.0;
}

// Going from 2D to 3D: give every node some random depth, proportional to
// the current size of the drawing, and let the forces do the rest.
void Layout::inflate (unsigned seed) {
  const size_t n = x_.size ();
  double sum = 0;
  for (size_t i = 0; i < n; i++)
    sum += (double) x_[i] * x_[i] + (double) y_[i] * y_[i];
  const float rms = n ? (float) std::sqrt (sum / (double) n) : 1.0f;
  std::mt19937 rng (seed);
  std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
  for (size_t i = 0; i < n; i++)
    z_[i] += 0.35f * rms * uni (rng);
  speed_efficiency_ = 1.0;
}

void Layout::run (unsigned seed) {
  using clock = std::chrono::steady_clock;
  unsigned shuffles = 0;
  while (!quit_.load ()) {
    const bool want_deep = three_d.load ();
    bool flattening = false;
    if (want_deep && !deep_) {
      deep_ = true;
      inflate (seed + 104729u * ++shuffles);
    } else if (!want_deep && deep_) {
      deep_ = false;
    }
    if (!deep_) {
      // Squash any remaining depth, also while the layout is frozen.
      float zmax = 0;
      for (float &z : z_) {
        z *= 0.86f;
        zmax = std::max (zmax, std::fabs (z));
      }
      if (zmax > 0) {
        flattening = true;
        if (zmax < 1e-3f)
          std::fill (z_.begin (), z_.end (), 0.0f);
      }
    }
    if (reshuffle_.exchange (false))
      scatter (seed + 7919u * ++shuffles);
    else if (!running.load () && !flattening) {
      std::this_thread::sleep_for (std::chrono::milliseconds (20));
      continue;
    }
    auto t0 = clock::now ();
    if (running.load ())
      iterate ();
    {
      std::lock_guard<std::mutex> lock (mutex_);
      const size_t n = x_.size ();
      for (size_t i = 0; i < n; i++) {
        published_[3 * i] = x_[i];
        published_[3 * i + 1] = y_[i];
        published_[3 * i + 2] = z_[i];
      }
    }
    if (running.load ())
      iterations.fetch_add (1);
    double ms = std::chrono::duration<double, std::milli> (clock::now () - t0).count ();
    ms_per_iteration.store ((float) ms);
    // Small graphs would otherwise spin through thousands of iterations per
    // second; 120 per second is plenty to look smooth.
    if (ms < 8.0)
      std::this_thread::sleep_for (std::chrono::microseconds ((long) ((8.0 - ms) * 1000)));
  }
}

/*------------------------------------------------------------------------*/
// Barnes-Hut tree: a quadtree in 2D, an octree in 3D.

void Layout::build_tree () {
  const int n = (int) x_.size ();
  const bool deep = deep_;
  const int fanout = deep ? 8 : 4;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (int i = 0; i < n; i++) {
    lo[0] = std::min (lo[0], x_[i]);
    hi[0] = std::max (hi[0], x_[i]);
    lo[1] = std::min (lo[1], y_[i]);
    hi[1] = std::max (hi[1], y_[i]);
    if (deep) {
      lo[2] = std::min (lo[2], z_[i]);
      hi[2] = std::max (hi[2], z_[i]);
    }
  }
  if (!deep)
    lo[2] = hi[2] = 0;
  tree_.clear ();
  tree_.reserve ((size_t) n * 3 + 16);
  TNode root;
  root.mx = root.my = root.mz = root.mass = 0;
  root.cx = 0.5f * (lo[0] + hi[0]);
  root.cy = 0.5f * (lo[1] + hi[1]);
  root.cz = 0.5f * (lo[2] + hi[2]);
  root.half = 0.5f * std::max (hi[0] - lo[0], std::max (hi[1] - lo[1], hi[2] - lo[2])) + 1e-3f;
  root.child = -1;
  root.body = -1;
  tree_.push_back (root);

  const std::vector<float> &mass = g_->mass;
  const int max_depth = 40;

  auto octant = [deep] (const TNode &q, float x, float y, float z) {
    return (x > q.cx ? 1 : 0) | (y > q.cy ? 2 : 0) | (deep && z > q.cz ? 4 : 0);
  };

  for (int i = 0; i < n; i++) {
    const float xi = x_[i], yi = y_[i], zi = deep ? z_[i] : 0.0f, mi = mass[i];
    int node = 0;
    int depth = 0;
    for (;;) {
      TNode *q = &tree_[(size_t) node];
      if (q->child < 0) {
        if (q->body == -1) { // empty leaf
          q->body = i;
          q->mx = mi * xi;
          q->my = mi * yi;
          q->mz = mi * zi;
          q->mass = mi;
          break;
        }
        if (q->body == -2 || depth >= max_depth) { // crowded leaf
          q->body = -2;
          q->mx += mi * xi;
          q->my += mi * yi;
          q->mz += mi * zi;
          q->mass += mi;
          break;
        }
        // Occupied leaf: split it and push the old body one level down.
        const int old = q->body;
        const int first = (int) tree_.size ();
        const float h = 0.5f * q->half, cx = q->cx, cy = q->cy, cz = q->cz;
        q->child = first;
        q->body = -1;
        for (int k = 0; k < fanout; k++) {
          TNode c;
          c.mx = c.my = c.mz = c.mass = 0;
          c.cx = cx + ((k & 1) ? h : -h);
          c.cy = cy + ((k & 2) ? h : -h);
          c.cz = deep ? cz + ((k & 4) ? h : -h) : 0.0f;
          c.half = h;
          c.child = -1;
          c.body = -1;
          tree_.push_back (c);
        }
        q = &tree_[(size_t) node]; // push_back may have moved the nodes
        const float xo = x_[old], yo = y_[old], zo = deep ? z_[old] : 0.0f;
        TNode &c = tree_[(size_t) (first + octant (*q, xo, yo, zo))];
        c.body = old;
        c.mx = mass[old] * xo;
        c.my = mass[old] * yo;
        c.mz = mass[old] * zo;
        c.mass = mass[old];
        // fall through: `q` is now an inner node holding the old body's mass
      }
      q->mx += mi * xi;
      q->my += mi * yi;
      q->mz += mi * zi;
      q->mass += mi;
      node = q->child + octant (*q, xi, yi, zi);
      depth++;
    }
  }
  for (TNode &q : tree_)
    if (q.mass > 0) {
      q.mx /= q.mass;
      q.my /= q.mass;
      q.mz /= q.mass;
    }
}

void Layout::repulse_range (int from, int to) {
  const float theta2 = 1.2f * 1.2f;
  const float kr = scaling.load ();
  const bool deep = deep_;
  const int fanout = deep ? 8 : 4;
  const std::vector<float> &mass = g_->mass;
  int stack[512];
  for (int i = from; i < to; i++) {
    const float xi = x_[i], yi = y_[i], zi = deep ? z_[i] : 0.0f, mi = mass[i];
    float fx = 0, fy = 0, fz = 0;
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
      const TNode &q = tree_[(size_t) stack[--sp]];
      if (q.mass <= 0 || q.body == i)
        continue;
      const float ddx = xi - q.mx, ddy = yi - q.my, ddz = zi - q.mz;
      const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
      const float size = 2.0f * q.half;
      if (q.child < 0 || size * size < theta2 * d2) {
        if (d2 > 1e-9f) {
          const float f = kr * mi * q.mass / d2;
          fx += ddx * f;
          fy += ddy * f;
          fz += ddz * f;
        }
      } else if (sp + fanout <= 512) {
        for (int k = 0; k < fanout; k++)
          stack[sp++] = q.child + k;
      }
    }
    dx_[i] += fx;
    dy_[i] += fy;
    dz_[i] += fz;
  }
}

void Layout::iterate () {
  const int n = (int) x_.size ();
  if (!n)
    return;
  const std::vector<float> &mass = g_->mass;
  const bool deep = deep_;

  for (int i = 0; i < n; i++) {
    odx_[i] = dx_[i];
    ody_[i] = dy_[i];
    odz_[i] = dz_[i];
    dx_[i] = dy_[i] = dz_[i] = 0;
  }

  // Repulsion between all pairs of nodes (approximated).
  build_tree ();
  if (workers_ <= 1)
    repulse_range (0, n);
  else {
    std::vector<std::thread> pool;
    const int chunk = (n + workers_ - 1) / workers_;
    for (int w = 0; w < workers_; w++) {
      const int from = w * chunk, to = std::min (n, from + chunk);
      if (from < to)
        pool.emplace_back ([this, from, to] { repulse_range (from, to); });
    }
    for (std::thread &t : pool)
      t.join ();
  }

  // Gravity keeps disconnected parts from drifting away.
  const float grav = gravity.load ();
  for (int i = 0; i < n; i++) {
    const float zi = deep ? z_[i] : 0.0f;
    const float d = std::sqrt (x_[i] * x_[i] + y_[i] * y_[i] + zi * zi);
    if (d > 1e-6f) {
      const float f = mass[i] * grav / d;
      dx_[i] -= x_[i] * f;
      dy_[i] -= y_[i] * f;
      dz_[i] -= zi * f;
    }
  }

  // Attraction along edges.
  const size_t m = g_->nedges ();
  const uint32_t *e = g_->edges.data ();
  const float *w = g_->weight.data ();
  for (size_t k = 0; k < m; k++) {
    const uint32_t a = e[2 * k], b = e[2 * k + 1];
    const float ex = (x_[a] - x_[b]) * w[k], ey = (y_[a] - y_[b]) * w[k];
    dx_[a] -= ex;
    dy_[a] -= ey;
    dx_[b] += ex;
    dy_[b] += ey;
    if (deep) {
      const float ez = (z_[a] - z_[b]) * w[k];
      dz_[a] -= ez;
      dz_[b] += ez;
    }
  }

  // Adaptive global speed: go fast while nodes move steadily, slow down
  // when they start to oscillate.
  double swing = 0, traction = 0;
  for (int i = 0; i < n; i++) {
    const double sx = odx_[i] - dx_[i], sy = ody_[i] - dy_[i], sz = odz_[i] - dz_[i];
    const double tx = odx_[i] + dx_[i], ty = ody_[i] + dy_[i], tz = odz_[i] + dz_[i];
    swing += mass[i] * std::sqrt (sx * sx + sy * sy + sz * sz);
    traction += 0.5 * mass[i] * std::sqrt (tx * tx + ty * ty + tz * tz);
  }
  if (swing <= 0 || traction <= 0)
    return;
  const double tolerance = 1.0;
  const double est = 0.05 * std::sqrt ((double) n);
  const double min_jt = std::sqrt (est), max_jt = 10.0;
  double jt = tolerance *
              std::max (min_jt, std::min (max_jt, est * traction / ((double) n * n)));
  const double min_eff = 0.05;
  if (swing / traction > 2.0) {
    if (speed_efficiency_ > min_eff)
      speed_efficiency_ *= 0.5;
    jt = std::max (jt, tolerance);
  }
  const double target = jt * speed_efficiency_ * traction / swing;
  if (swing > jt * traction) {
    if (speed_efficiency_ > min_eff)
      speed_efficiency_ *= 0.7;
  } else if (speed_ < 1000.0)
    speed_efficiency_ *= 1.3;
  speed_ += std::min (target - speed_, 0.5 * speed_);

  for (int i = 0; i < n; i++) {
    const double sx = odx_[i] - dx_[i], sy = ody_[i] - dy_[i], sz = odz_[i] - dz_[i];
    const double sw = mass[i] * std::sqrt (sx * sx + sy * sy + sz * sz);
    const double factor = speed_ / (1.0 + std::sqrt (speed_ * sw));
    x_[i] += (float) (dx_[i] * factor);
    y_[i] += (float) (dy_[i] * factor);
    if (deep)
      z_[i] += (float) (dz_[i] * factor);
  }
}
