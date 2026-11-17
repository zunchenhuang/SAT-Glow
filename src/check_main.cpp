// satglow-check: runs a hooked solver without graphics.
//
// Prints the event counters and checks that the hooks are consistent; can
// also record a trace, compute search metrics, stop after a time limit, and
// print one CSV line for benchmark scripts.  The same source is also built
// as satglow-check-base, against solvers with all hooks compiled out, which
// is the baseline for overhead measurements.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "clusters.hpp"
#include "graph.hpp"
#include "metrics.hpp"
#include "solver.hpp"
#include "trace.hpp"

#ifdef SATGLOW_BASE
static const char *BUILD = "base";
#else
static const char *BUILD = "hooked";
#endif

static void usage () {
  printf ("usage: satglow-check [options] FILE.cnf[.xz|.gz|.bz2] [name=value ...]\n"
          "\n"
          "  --solver NAME     cadical (default), kissat or minisat\n"
          "  --timeout S       stop the solver after S seconds (result UNKNOWN)\n"
          "  --no-hooks        run with the hooks idle (no context attached)\n"
          "  --trace FILE      record every solver event; .zst, .xz or .gz compresses\n"
          "  --metrics FILE    write search metrics as JSON\n"
          "  --timeseries FILE write metrics every N conflicts as CSV\n"
          "  --interval N      conflicts per time-series row (default 1000)\n"
          "  --csv             print one CSV line (see --csv-header) instead of a report\n"
          "  --csv-header      print the CSV header and exit\n"
          "  name=value        solver option, e.g. elim=0\n");
}

static const char *CSV_HEADER =
    "instance,solver,build,mode,result,seconds,own_conflicts,own_decisions,hook_conflicts,hook_events,"
    "trace_bytes,hook_check,model_check";

int main (int argc, char **argv) {
  int solver = 0;
  double timeout = 0;
  bool no_hooks = false, csv = false;
  std::string file, trace_path, metrics_path, ts_path;
  uint64_t interval = 1000;
  std::vector<std::pair<std::string, int>> opts;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto need = [&] () -> const char * {
      if (i + 1 >= argc) {
        fprintf (stderr, "satglow-check: %s needs an argument\n", a.c_str ());
        exit (2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help")
      return usage (), 0;
    else if (a == "--csv-header")
      return printf ("%s\n", CSV_HEADER), 0;
    else if (a == "--solver") {
      const char *s = need ();
      solver = solver_index (s);
      if (solver < 0) {
        fprintf (stderr, "satglow-check: unknown solver '%s' (cadical, kissat, minisat)\n", s);
        return 2;
      }
    } else if (a == "--timeout")
      timeout = atof (need ());
    else if (a == "--no-hooks")
      no_hooks = true;
    else if (a == "--trace")
      trace_path = need ();
    else if (a == "--metrics")
      metrics_path = need ();
    else if (a == "--timeseries")
      ts_path = need ();
    else if (a == "--interval")
      interval = (uint64_t) atoll (need ());
    else if (a == "--csv")
      csv = true;
    else if (a.find ('=') != std::string::npos && a[0] != '-') {
      const size_t eq = a.find ('=');
      opts.push_back ({a.substr (0, eq), atoi (a.c_str () + eq + 1)});
    } else if (!a.empty () && a[0] == '-') {
      fprintf (stderr, "satglow-check: unknown option '%s'\n", a.c_str ());
      return 2;
    } else
      file = a;
  }
  if (file.empty ())
    return usage (), 2;
  const bool observe = !trace_path.empty () || !metrics_path.empty () || !ts_path.empty ();
#ifdef SATGLOW_BASE
  if (observe) {
    fprintf (stderr, "satglow-check-base: hooks are compiled out, use satglow-check for traces and metrics\n");
    return 2;
  }
#endif
  if (observe && no_hooks) {
    fprintf (stderr, "satglow-check: --no-hooks cannot record traces or metrics\n");
    return 2;
  }

  Cnf cnf;
  std::string err;
  if (!parse_dimacs (file, cnf, err)) {
    fprintf (stderr, "satglow-check: %s\n", err.c_str ());
    return 2;
  }

  // Structure for the metrics (outside the timed part).
  Graph graph;
  Clustering clu;
  double structure_seconds = 0;
  if (!metrics_path.empty () || !ts_path.empty ()) {
    auto t0 = std::chrono::steady_clock::now ();
    build_vig (cnf, graph);
    build_clustering (graph, clu);
    structure_seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
  }

  EventFanout fanout;
  TraceWriter writer;
  std::unique_ptr<MetricsAnalyzer> analyzer;
  SolverRunner runner;
  if (!trace_path.empty ()) {
    TraceHeader h;
    h.nvars = cnf.nvars;
    h.cnf_hash = cnf_fingerprint (cnf);
    snprintf (h.solver, sizeof h.solver, "%s", make_backend (solver)->name ());
    snprintf (h.cnf_name, sizeof h.cnf_name, "%s", cnf.name.c_str ());
    if (!writer.open (trace_path, h, err)) {
      fprintf (stderr, "satglow-check: %s\n", err.c_str ());
      return 2;
    }
    fanout.sinks.push_back (&writer);
  }
  if (!metrics_path.empty () || !ts_path.empty ()) {
    analyzer.reset (new MetricsAnalyzer (cnf, &clu));
    if (!ts_path.empty () && !analyzer->open_timeseries (ts_path, interval, err)) {
      fprintf (stderr, "satglow-check: %s\n", err.c_str ());
      return 2;
    }
    fanout.sinks.push_back (analyzer.get ());
  }

  Shared sh;
  sh.unlimited.store (true);
  if (!fanout.sinks.empty ())
    sh.trace_sink = &fanout;
  runner.hooks = !no_hooks;
  runner.start (cnf, sh, solver, opts);
  auto t0 = std::chrono::steady_clock::now ();
  while (sh.status.load () < ST_ABORTED) {
    std::this_thread::sleep_for (std::chrono::milliseconds (2));
    if (timeout > 0 && std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count () > timeout)
      break;
  }
  runner.stop ();

  const int st = sh.status.load ();
  const int result = st == ST_SAT ? 10 : st == ST_UNSAT ? 20 : 0;
  if (!fanout.sinks.empty ()) {
    sh.flush_trace ();
    std::vector<uint32_t> tail;
    trace_outcome_records (result, sh.state, cnf.nvars, tail);
    fanout.consume (tail.data (), tail.size ());
  }
  bool trace_ok = true;
  if (!trace_path.empty ())
    trace_ok = writer.close ();

  const sg_ctx &c = sh.ctx;
  const char *res = result == 10 ? "SATISFIABLE" : result == 20 ? "UNSATISFIABLE" : "UNKNOWN";
  const char *mode = no_hooks ? "idle" : !trace_path.empty () ? "trace" : analyzer ? "metrics" : "viz";
#ifdef SATGLOW_BASE
  mode = "none";
#endif
  const long long he = sh.hook_errors.load ();
  const bool model_ok = result != 10 || sh.model_ok.load ();

  if (analyzer) {
    char extra[1024];
    snprintf (extra, sizeof extra,
              ",\n  \"instance\": \"%s\",\n  \"solver\": \"%s\",\n  \"result\": \"%s\",\n"
              "  \"solve_seconds\": %.3f,\n  \"structure_seconds\": %.3f,\n  \"own_conflicts\": %llu,\n"
              "  \"own_decisions\": %llu",
              cnf.name.c_str (), runner.solver_name.c_str (), res, sh.solve_seconds.load (), structure_seconds,
              (unsigned long long) runner.own_conflicts, (unsigned long long) runner.own_decisions);
    if (!metrics_path.empty () && !analyzer->write_json (metrics_path, extra, err))
      fprintf (stderr, "satglow-check: %s\n", err.c_str ());
    analyzer.reset (); // closes the time series
  }

  if (csv) {
    printf ("%s,%s,%s,%s,%s,%.4f,%llu,%llu,%llu,%llu,%llu,%s,%s\n", cnf.name.c_str (), SOLVER_NAMES[solver], BUILD,
            mode, result == 10 ? "SAT" : result == 20 ? "UNSAT" : "UNKNOWN", sh.solve_seconds.load (),
            (unsigned long long) runner.own_conflicts, (unsigned long long) runner.own_decisions,
            (unsigned long long) c.conflicts, (unsigned long long) (c.assigns + c.unassigns),
            (unsigned long long) writer.bytes (), he ? "FAILED" : "ok", model_ok ? "ok" : "FAILED");
  } else {
    printf ("instance      %s (%d variables, %zu clauses)\n", cnf.name.c_str (), cnf.nvars, cnf.nclauses ());
    printf ("solver        %s (%s build, hooks %s)\n", runner.solver_name.c_str (), BUILD, mode);
    printf ("result        %s in %.3f s\n", res, sh.solve_seconds.load ());
    printf ("solver stats  %llu conflicts, %llu decisions (the solver's own counters)\n",
            (unsigned long long) runner.own_conflicts, (unsigned long long) runner.own_decisions);
    printf ("assignments   %llu (decisions %llu, propagations %llu, inprocessing %llu)\n",
            (unsigned long long) c.assigns, (unsigned long long) c.decisions, (unsigned long long) c.propagations,
            (unsigned long long) c.inprocessing);
    printf ("unassignments %llu\n", (unsigned long long) c.unassigns);
    printf ("conflicts     %llu\n", (unsigned long long) c.conflicts);
    printf ("backjumps     %llu (average distance %.2f levels)\n", (unsigned long long) c.backjumps,
            c.backjumps ? (double) c.backjump_dist / (double) c.backjumps : 0.0);
    printf ("restarts      %llu, reductions %llu\n", (unsigned long long) c.restarts,
            (unsigned long long) c.reductions);
    printf ("learned       %llu clauses (average size %.1f, average glue %.1f)\n", (unsigned long long) c.learned,
            c.learned ? (double) c.learned_lits / (double) c.learned : 0.0,
            c.learned ? (double) c.learned_glue / (double) c.learned : 0.0);
    printf ("trail at end  %lld assigned, %lld fixed at level 0, max level %d\n", (long long) c.trail,
            (long long) c.fixed, c.max_level);
    printf ("preprocessing %lld variables eliminated, substituted or pure\n", (long long) c.gone);
    if (!trace_path.empty ())
      printf ("trace         %s (%.1f MB before compression)%s\n", trace_path.c_str (), writer.bytes () / 1e6,
              trace_ok ? "" : " WRITE FAILED");
    if (!metrics_path.empty ())
      printf ("metrics       %s (%d communities, structure in %.2f s)\n", metrics_path.c_str (), clu.k,
              structure_seconds);
    printf ("hook check    %s\n", he ? "FAILED" : "ok");
    if (result == 10)
      printf ("model check   %s (%lld variables changed by model reconstruction)\n", model_ok ? "ok" : "FAILED",
              (long long) sh.model_repaired.load ());
  }
  return he || !model_ok || !trace_ok ? 1 : 0;
}
