// satglow-metrics: computes search metrics from a recorded trace.
//
//   satglow-metrics TRACE FILE.cnf --metrics out.json [--timeseries out.csv]
//                   [--interval N]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clusters.hpp"
#include "graph.hpp"
#include "metrics.hpp"
#include "trace.hpp"

int main (int argc, char **argv) {
  std::string trace_path, cnf_path, metrics_path, ts_path;
  uint64_t interval = 1000;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if ((a == "--metrics" || a == "--timeseries" || a == "--interval") && i + 1 < argc) {
      const char *v = argv[++i];
      if (a == "--metrics")
        metrics_path = v;
      else if (a == "--timeseries")
        ts_path = v;
      else
        interval = (uint64_t) atoll (v);
    } else if (a[0] == '-') {
      fprintf (stderr, "usage: satglow-metrics TRACE FILE.cnf --metrics OUT.json [--timeseries OUT.csv] [--interval N]\n");
      return 2;
    } else if (trace_path.empty ())
      trace_path = a;
    else
      cnf_path = a;
  }
  if (trace_path.empty () || cnf_path.empty () || (metrics_path.empty () && ts_path.empty ())) {
    fprintf (stderr, "usage: satglow-metrics TRACE FILE.cnf --metrics OUT.json [--timeseries OUT.csv] [--interval N]\n");
    return 2;
  }
  std::string err;
  Cnf cnf;
  if (!parse_dimacs (cnf_path, cnf, err)) {
    fprintf (stderr, "satglow-metrics: %s\n", err.c_str ());
    return 2;
  }
  TraceHeader h;
  TraceReader reader;
  if (!reader.open (trace_path, h, err)) {
    fprintf (stderr, "satglow-metrics: %s\n", err.c_str ());
    return 2;
  }
  if (h.nvars != cnf.nvars || h.cnf_hash != cnf_fingerprint (cnf)) {
    fprintf (stderr, "satglow-metrics: trace was recorded on a different formula (%s)\n", h.cnf_name);
    return 2;
  }
  Graph graph;
  Clustering clu;
  build_vig (cnf, graph);
  build_clustering (graph, clu);
  MetricsAnalyzer an (cnf, &clu);
  if (!ts_path.empty () && !an.open_timeseries (ts_path, interval, err)) {
    fprintf (stderr, "satglow-metrics: %s\n", err.c_str ());
    return 2;
  }
  std::vector<uint32_t> buf (1u << 20);
  TraceOutcome out;
  size_t n;
  uint64_t words = 0;
  while ((n = reader.read (buf.data (), buf.size ())) > 0) {
    an.consume (buf.data (), n);
    // pick up the result records
    for (size_t i = 0; i + 1 < n; i += 2)
      if ((buf[i] >> 28) == 14)
        out.result = (int) buf[i + 1];
    words += n;
  }
  char extra[512];
  snprintf (extra, sizeof extra, ",\n  \"instance\": \"%s\",\n  \"solver\": \"%s\",\n  \"result\": \"%s\"", h.cnf_name,
            h.solver, out.result == 10 ? "SATISFIABLE" : out.result == 20 ? "UNSATISFIABLE" : "UNKNOWN");
  if (!metrics_path.empty () && !an.write_json (metrics_path, extra, err)) {
    fprintf (stderr, "satglow-metrics: %s\n", err.c_str ());
    return 2;
  }
  printf ("satglow-metrics: %llu events, %llu conflicts, %d communities\n", (unsigned long long) (words / 2),
          (unsigned long long) an.conflicts (), clu.k);
  return 0;
}
