#include "clusters.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>

namespace {

struct Edge {
  uint32_t a, b;
  double w;
};

// One level of Louvain: local moving until (almost) nothing moves.
// Returns the community of every node, numbered 0 .. count-1.
std::vector<int> louvain_level (int n, const std::vector<Edge> &edges, const std::vector<double> &self,
                                std::mt19937 &rng, int &count) {
  std::vector<uint32_t> off ((size_t) n + 1, 0), adj;
  std::vector<double> wt;
  for (const Edge &e : edges)
    off[e.a + 1]++, off[e.b + 1]++;
  for (int i = 0; i < n; i++)
    off[(size_t) i + 1] += off[(size_t) i];
  adj.resize (off.back ());
  wt.resize (off.back ());
  std::vector<uint32_t> fill (off.begin (), off.end () - 1);
  std::vector<double> k ((size_t) n, 0.0);
  for (int i = 0; i < n; i++)
    k[(size_t) i] = 2.0 * self[(size_t) i];
  for (const Edge &e : edges) {
    adj[fill[e.a]] = e.b, wt[fill[e.a]++] = e.w;
    adj[fill[e.b]] = e.a, wt[fill[e.b]++] = e.w;
    k[e.a] += e.w, k[e.b] += e.w;
  }
  double m2 = 0;
  for (double x : k)
    m2 += x;
  std::vector<int> comm ((size_t) n);
  std::iota (comm.begin (), comm.end (), 0);
  if (m2 <= 0) {
    count = n;
    return comm;
  }
  std::vector<double> tot (k);
  std::vector<double> nw ((size_t) n, -1.0); // weight from the node to each community
  std::vector<int> touched;
  std::vector<int> order ((size_t) n);
  std::iota (order.begin (), order.end (), 0);
  std::shuffle (order.begin (), order.end (), rng);

  for (int pass = 0; pass < 20; pass++) {
    long moved = 0;
    for (int i : order) {
      const int c = comm[(size_t) i];
      touched.clear ();
      nw[(size_t) c] = 0;
      touched.push_back (c);
      for (uint32_t e = off[(size_t) i]; e < off[(size_t) i + 1]; e++) {
        const int cc = comm[adj[e]];
        if (nw[(size_t) cc] < 0) {
          nw[(size_t) cc] = 0;
          touched.push_back (cc);
        }
        nw[(size_t) cc] += wt[e];
      }
      const double ki = k[(size_t) i];
      tot[(size_t) c] -= ki;
      int best = c;
      double best_gain = nw[(size_t) c] - tot[(size_t) c] * ki / m2;
      for (int cc : touched) {
        const double gain = nw[(size_t) cc] - tot[(size_t) cc] * ki / m2;
        if (gain > best_gain + 1e-12)
          best = cc, best_gain = gain;
      }
      tot[(size_t) best] += ki;
      if (best != c)
        comm[(size_t) i] = best, moved++;
      for (int cc : touched)
        nw[(size_t) cc] = -1.0;
    }
    if (moved == 0 || moved < n / 2000)
      break;
  }
  std::vector<int> id ((size_t) n, -1);
  count = 0;
  for (int &c : comm) {
    if (id[(size_t) c] < 0)
      id[(size_t) c] = count++;
    c = id[(size_t) c];
  }
  return comm;
}

} // namespace

void build_clustering (const Graph &g, Clustering &c) {
  auto t0 = std::chrono::steady_clock::now ();
  c = Clustering ();
  const int n = g.n;
  c.of.resize ((size_t) n);
  std::iota (c.of.begin (), c.of.end (), 0);

  std::vector<Edge> edges;
  edges.reserve (g.nedges ());
  for (size_t e = 0; e < g.nedges (); e++)
    edges.push_back ({g.edges[2 * e], g.edges[2 * e + 1], (double) g.weight[e]});
  std::vector<double> self ((size_t) n, 0.0);
  std::mt19937 rng (4242);
  int cur = n;

  // Repeat: move nodes between communities, then merge each community
  // into a single node, until nothing changes any more.
  for (int level = 0; level < 10 && cur > 1; level++) {
    int count = 0;
    std::vector<int> comm = louvain_level (cur, edges, self, rng, count);
    if (count == cur)
      break;
    c.levels++;
    for (int &x : c.of)
      x = comm[(size_t) x];
    std::vector<double> nself ((size_t) count, 0.0);
    for (int i = 0; i < cur; i++)
      nself[(size_t) comm[(size_t) i]] += self[(size_t) i];
    std::vector<std::pair<uint64_t, double>> agg;
    agg.reserve (edges.size ());
    for (const Edge &e : edges) {
      uint32_t a = (uint32_t) comm[e.a], b = (uint32_t) comm[e.b];
      if (a == b)
        nself[a] += e.w;
      else {
        if (a > b)
          std::swap (a, b);
        agg.push_back ({((uint64_t) a << 32) | b, e.w});
      }
    }
    std::sort (agg.begin (), agg.end (),
               [] (const std::pair<uint64_t, double> &x, const std::pair<uint64_t, double> &y) { return x.first < y.first; });
    edges.clear ();
    for (size_t i = 0; i < agg.size ();) {
      size_t j = i;
      double w = 0;
      while (j < agg.size () && agg[j].first == agg[i].first)
        w += agg[j++].second;
      edges.push_back ({(uint32_t) (agg[i].first >> 32), (uint32_t) (agg[i].first & 0xffffffffu), w});
      i = j;
    }
    self.swap (nself);
    cur = count;
  }
  c.k = cur;

  // Modularity of the result (just for the panel).
  {
    std::vector<double> k ((size_t) cur, 0.0);
    double m2 = 0;
    for (int i = 0; i < cur; i++)
      k[(size_t) i] = 2.0 * self[(size_t) i];
    for (const Edge &e : edges)
      k[e.a] += e.w, k[e.b] += e.w;
    for (double x : k)
      m2 += x;
    if (m2 > 0)
      for (int i = 0; i < cur; i++)
        c.modularity += 2.0 * self[(size_t) i] / m2 - (k[(size_t) i] / m2) * (k[(size_t) i] / m2);
  }

  // Graph of clusters for the force layout.  Masses grow with the size of
  // the cluster so that big clusters claim more room.
  c.size.assign ((size_t) cur, 0);
  for (int x : c.of)
    c.size[(size_t) x]++;
  c.cgraph = Graph ();
  c.cgraph.n = cur;
  c.cgraph.mass.resize ((size_t) cur);
  for (int i = 0; i < cur; i++)
    c.cgraph.mass[(size_t) i] = 1.0f + std::sqrt ((float) c.size[(size_t) i]);
  for (const Edge &e : edges) {
    c.cgraph.edges.push_back (e.a);
    c.cgraph.edges.push_back (e.b);
    c.cgraph.weight.push_back ((float) std::sqrt (e.w));
  }

  // Places inside the clusters: breadth-first order from the best connected
  // member, laid out along a sunflower spiral (2D) or a spiral ball (3D).
  std::vector<uint32_t> off, adj;
  build_adjacency (g, off, adj);
  std::vector<std::vector<uint32_t>> members ((size_t) cur);
  for (int v = 0; v < n; v++)
    members[(size_t) c.of[(size_t) v]].push_back ((uint32_t) v);
  std::vector<char> seen ((size_t) n, 0);
  c.off2.assign ((size_t) n * 2, 0.0f);
  c.off3.assign ((size_t) n * 3, 0.0f);
  std::vector<uint32_t> order;
  const float golden = 2.39996323f;
  for (int cl = 0; cl < cur; cl++) {
    std::vector<uint32_t> &mem = members[(size_t) cl];
    std::sort (mem.begin (), mem.end (), [&off] (uint32_t x, uint32_t y) {
      return off[x + 1] - off[x] > off[y + 1] - off[y];
    });
    order.clear ();
    for (uint32_t start : mem) {
      if (seen[start])
        continue;
      seen[start] = 1;
      size_t head = order.size ();
      order.push_back (start);
      while (head < order.size ()) {
        const uint32_t u = order[head++];
        for (uint32_t e = off[u]; e < off[u + 1]; e++) {
          const uint32_t x = adj[e];
          if (!seen[x] && c.of[x] == cl) {
            seen[x] = 1;
            order.push_back (x);
          }
        }
      }
    }
    const float s = (float) order.size ();
    for (size_t t = 0; t < order.size (); t++) {
      const uint32_t v = order[t];
      const float q = ((float) t + 0.5f) / s;
      const float th = golden * (float) t;
      const float r2 = std::sqrt (q);
      c.off2[2 * v] = r2 * std::cos (th);
      c.off2[2 * v + 1] = r2 * std::sin (th);
      const float r3 = std::cbrt (q);
      const float z = 1.0f - 2.0f * q;
      const float rho = std::sqrt (std::max (0.0f, 1.0f - z * z));
      c.off3[3 * v] = r3 * rho * std::cos (th);
      c.off3[3 * v + 1] = r3 * rho * std::sin (th);
      c.off3[3 * v + 2] = r3 * z;
    }
  }
  c.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
}

void expand_positions (const Clustering &c, const std::vector<float> &cpos, float depth_mix, float spread,
                       std::vector<float> &pos, std::vector<float> &rad) {
  const size_t n = c.of.size ();
  pos.resize (n * 3);
  if (cpos.size () < (size_t) c.k * 3)
    return;
  // Size of the drawing: size-weighted spread of the cluster centres.
  double mx = 0, my = 0, mz = 0, tot = 0;
  for (int i = 0; i < c.k; i++) {
    const double s = c.size[(size_t) i];
    mx += s * cpos[3 * (size_t) i], my += s * cpos[3 * (size_t) i + 1], mz += s * cpos[3 * (size_t) i + 2];
    tot += s;
  }
  mx /= tot, my /= tot, mz /= tot;
  double var = 0;
  for (int i = 0; i < c.k; i++) {
    const double dx = cpos[3 * (size_t) i] - mx, dy = cpos[3 * (size_t) i + 1] - my, dz = cpos[3 * (size_t) i + 2] - mz;
    var += c.size[(size_t) i] * (dx * dx + dy * dy + dz * dz);
  }
  float R = (float) std::sqrt (var / tot);
  if (c.k < 3 || R < 1e-3f)
    R = 10.0f * std::sqrt ((float) n);
  // Cluster radius: the clusters together cover about a quarter of the
  // drawing, each in proportion to its size.
  rad.resize ((size_t) c.k);
  for (int i = 0; i < c.k; i++) {
    const float f = (float) c.size[(size_t) i] / (float) n;
    rad[(size_t) i] = spread * R * ((1.0f - depth_mix) * 0.55f * std::sqrt (f) + depth_mix * 0.50f * std::cbrt (f));
  }
  for (size_t v = 0; v < n; v++) {
    const size_t cl = (size_t) c.of[v];
    const float r = rad[cl];
    const float ox = c.off2[2 * v] + (c.off3[3 * v] - c.off2[2 * v]) * depth_mix;
    const float oy = c.off2[2 * v + 1] + (c.off3[3 * v + 1] - c.off2[2 * v + 1]) * depth_mix;
    const float oz = c.off3[3 * v + 2] * depth_mix;
    pos[3 * v] = cpos[3 * cl] + r * ox;
    pos[3 * v + 1] = cpos[3 * cl + 1] + r * oy;
    pos[3 * v + 2] = cpos[3 * cl + 2] + r * oz;
  }
}
