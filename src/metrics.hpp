// Search metrics computed from the solver's event stream, either while the
// solver runs (as an EventSink) or afterwards from a trace file.
//
// Besides plain counts, the analyzer relates the search to the structure of
// the formula: the variable graph is split into communities (Louvain), and
// every learned clause is compared with a random clause of the same size.
// "Locality" > 1 means learned clauses span fewer communities than chance.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "clusters.hpp"
#include "cnf.hpp"
#include "graph.hpp"
#include "trace.hpp"

class MetricsAnalyzer : public EventSink {
public:
  // `clu` may be null: then no community metrics are computed.
  MetricsAnalyzer (const Cnf &cnf, const Clustering *clu);
  ~MetricsAnalyzer () override;

  // Time series: one CSV row every `interval` conflicts (0: none).
  bool open_timeseries (const std::string &path, uint64_t interval, std::string &err);

  void consume (const uint32_t *words, size_t n) override;

  // Writes the summary as JSON.  `extra` is inserted verbatim as further
  // top-level fields (it must start with a comma or be empty).
  bool write_json (const std::string &path, const std::string &extra, std::string &err);

  uint64_t conflicts () const { return conflicts_; }

private:
  void finish_clause ();
  void row ();
  double expected_span (size_t k);
  double expected_single (size_t k);

  const Cnf &cnf_;
  const Clustering *clu_;
  int n_;

  // event counts
  uint64_t decisions_ = 0, propagations_ = 0, inprocess_ = 0, unassigns_ = 0, conflicts_ = 0,
           backjumps_ = 0, restarts_ = 0, reductions_ = 0, learned_ = 0, gone_ = 0, backtracks_ = 0;
  // mirrored search state
  std::vector<uint8_t> assigned_;
  int64_t trail_ = 0;
  int level_ = 0, max_level_ = 0;
  // backjumps
  uint64_t jump_dist_ = 0, jump_chrono_ = 0;
  std::vector<uint64_t> jump_hist_; // bins 0, 1, 2, 3-4, 5-8, ...
  // state at conflicts
  double sum_conf_level_ = 0, sum_conf_trail_ = 0;
  // learned clauses
  size_t want_ = 0;
  uint32_t cur_glue_ = 0;
  std::vector<uint32_t> cur_; // variables of the clause being received
  double sum_size_ = 0, sum_glue_ = 0;
  double sum_span_ = 0, sum_exp_span_ = 0, sum_single_ = 0, sum_exp_single_ = 0;
  uint64_t clauses_with_span_ = 0, focus_switches_ = 0;
  int last_focus_ = -1;
  std::vector<int> mark_;    // per community: last clause that touched it
  std::vector<int> touch_;   // per community: hits in the current clause
  int stamp_ = 0;
  std::vector<double> exp_span_memo_, exp_single_memo_;
  std::vector<double> lg_; // log-factorials 0..n
  // activity
  std::vector<uint32_t> var_assigns_, var_learnt_;
  std::vector<uint64_t> com_learnt_;
  // time series
  FILE *ts_ = nullptr;
  uint64_t ts_interval_ = 0, next_row_ = 0;
  struct Window {
    uint64_t clauses = 0;
    double size = 0, glue = 0, span = 0, exp_span = 0, single = 0, exp_single = 0;
  } win_;
};
