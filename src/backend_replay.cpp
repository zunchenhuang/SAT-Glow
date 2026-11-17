// Replay "backend": instead of solving, feeds a recorded trace through the
// hooks, so the visualizer shows the recorded run exactly as it happened.
#include <atomic>
#include <cstdio>

#include "solver.hpp"
#include "trace.hpp"

namespace {

struct ReplayBackend : SolverBackend {
  std::string path, label;
  TraceHeader header;
  TraceReader reader;
  TraceOutcome outcome;
  std::atomic<bool> stop{false};
  bool ok = false;

  explicit ReplayBackend (const std::string &p) : path (p) {
    std::string err;
    ok = reader.open (path, header, err);
    if (!ok)
      fprintf (stderr, "satglow: %s\n", err.c_str ());
    label = std::string ("Replay: ") + (ok ? header.solver : "(no trace)");
  }
  const char *name () const override { return label.c_str (); }
  bool set_option (const std::string &, int) override { return false; }
  void load (const Cnf &cnf) override {
    if (!ok)
      return;
    if (header.nvars != cnf.nvars) {
      fprintf (stderr, "satglow: trace has %d variables, formula has %d: not the same formula\n", header.nvars,
               cnf.nvars);
      ok = false;
    } else if (header.cnf_hash != cnf_fingerprint (cnf))
      fprintf (stderr, "satglow: warning: trace was recorded on a different version of '%s'\n", header.cnf_name);
  }
  int solve () override {
    if (!ok)
      return 0;
    std::vector<uint32_t> buf (1u << 16);
    size_t n;
    while (!stop.load (std::memory_order_relaxed) && (n = reader.read (buf.data (), buf.size ())) > 0)
      replay_events (buf.data (), n, outcome);
    return stop.load () ? 0 : outcome.result;
  }
  bool value (int var) override {
    const size_t w = (size_t) (var - 1) / 32;
    return w < outcome.model.size () && ((outcome.model[w] >> ((var - 1) % 32)) & 1u);
  }
  void terminate () override { stop.store (true); }
  void own_counts (uint64_t &c, uint64_t &d) override { c = d = 0; }
};

} // namespace

std::unique_ptr<SolverBackend> make_replay_backend (const std::string &path) {
  return std::unique_ptr<SolverBackend> (new ReplayBackend (path));
}
