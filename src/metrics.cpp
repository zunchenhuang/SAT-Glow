#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sg_hooks.h"

MetricsAnalyzer::MetricsAnalyzer (const Cnf &cnf, const Clustering *clu)
    : cnf_ (cnf), clu_ (clu), n_ (cnf.nvars) {
  assigned_.assign ((size_t) n_ + 1, 0);
  var_assigns_.assign ((size_t) n_ + 1, 0);
  var_learnt_.assign ((size_t) n_ + 1, 0);
  jump_hist_.assign (24, 0);
  if (clu_) {
    mark_.assign ((size_t) clu_->k, -1);
    touch_.assign ((size_t) clu_->k, 0);
    com_learnt_.assign ((size_t) clu_->k, 0);
    lg_.resize ((size_t) n_ + 2);
    lg_[0] = 0;
    for (int i = 1; i <= n_ + 1; i++)
      lg_[(size_t) i] = lg_[(size_t) i - 1] + std::log ((double) i);
  }
}

MetricsAnalyzer::~MetricsAnalyzer () {
  if (ts_)
    fclose (ts_);
}

bool MetricsAnalyzer::open_timeseries (const std::string &path, uint64_t interval, std::string &err) {
  ts_ = fopen (path.c_str (), "w");
  if (!ts_) {
    err = "cannot write '" + path + "'";
    return false;
  }
  ts_interval_ = interval ? interval : 1000;
  next_row_ = ts_interval_;
  fprintf (ts_, "conflicts,decisions,propagations,inprocessing,unassignments,restarts,reductions,"
                "trail,level,learned_size,learned_glue,span,expected_span,single_community,expected_single\n");
  return true;
}

// Probability that a random k-subset of the n variables misses a community
// of size s, i.e. C(n-s, k) / C(n, k).
static double miss_prob (const std::vector<double> &lg, int n, int s, size_t k) {
  if ((size_t) (n - s) < k)
    return 0.0;
  const double l = lg[(size_t) (n - s)] - lg[(size_t) (n - s) - k] - lg[(size_t) n] + lg[(size_t) n - k];
  return std::exp (l);
}

double MetricsAnalyzer::expected_span (size_t k) {
  if (k >= exp_span_memo_.size ())
    exp_span_memo_.resize (k + 1, -1.0);
  double &m = exp_span_memo_[k];
  if (m < 0) {
    double e = 0;
    for (int c = 0; c < clu_->k; c++)
      e += 1.0 - miss_prob (lg_, n_, clu_->size[(size_t) c], k);
    m = e;
  }
  return m;
}

double MetricsAnalyzer::expected_single (size_t k) {
  if (k >= exp_single_memo_.size ())
    exp_single_memo_.resize (k + 1, -1.0);
  double &m = exp_single_memo_[k];
  if (m < 0) {
    // sum over communities of C(s, k) / C(n, k)
    double p = 0;
    for (int c = 0; c < clu_->k; c++) {
      const int s = clu_->size[(size_t) c];
      if ((size_t) s < k)
        continue;
      p += std::exp (lg_[(size_t) s] - lg_[(size_t) s - k] - lg_[(size_t) n_] + lg_[(size_t) n_ - k]);
    }
    m = p;
  }
  return m;
}

void MetricsAnalyzer::finish_clause () {
  const size_t k = cur_.size ();
  if (!k) {
    want_ = 0;
    return;
  }
  learned_++;
  sum_size_ += (double) k;
  sum_glue_ += cur_glue_;
  win_.clauses++;
  win_.size += (double) k;
  win_.glue += cur_glue_;
  for (uint32_t v : cur_)
    var_learnt_[v]++;
  if (clu_ && k <= (size_t) n_) {
    stamp_++;
    int span = 0, focus = -1, best = 0;
    for (uint32_t v : cur_) {
      const int c = clu_->of[(size_t) v - 1];
      if (mark_[(size_t) c] != stamp_) {
        mark_[(size_t) c] = stamp_;
        touch_[(size_t) c] = 0;
        span++;
      }
      com_learnt_[(size_t) c]++;
      if (++touch_[(size_t) c] > best)
        best = touch_[(size_t) c], focus = c;
    }
    const double es = expected_span (k), e1 = expected_single (k);
    sum_span_ += span;
    sum_exp_span_ += es;
    sum_single_ += span == 1 ? 1.0 : 0.0;
    sum_exp_single_ += e1;
    win_.span += span;
    win_.exp_span += es;
    win_.single += span == 1 ? 1.0 : 0.0;
    win_.exp_single += e1;
    clauses_with_span_++;
    if (last_focus_ >= 0 && focus != last_focus_)
      focus_switches_++;
    last_focus_ = focus;
  }
  cur_.clear ();
  want_ = 0;
}

void MetricsAnalyzer::row () {
  const double c = win_.clauses ? (double) win_.clauses : 1.0;
  fprintf (ts_, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f\n",
           (unsigned long long) conflicts_, (unsigned long long) decisions_, (unsigned long long) propagations_,
           (unsigned long long) inprocess_, (unsigned long long) unassigns_, (unsigned long long) restarts_,
           (unsigned long long) reductions_, (long long) trail_, level_, win_.size / c, win_.glue / c,
           win_.span / c, win_.exp_span / c, win_.single / c,
           win_.exp_single / c);
  win_ = Window ();
}

void MetricsAnalyzer::consume (const uint32_t *w, size_t n) {
  for (size_t i = 0; i + 1 < n; i += 2) {
    const uint32_t head = w[i], val = w[i + 1];
    const uint32_t ev = head >> 28, pay = head & SG_PAYLOAD_MASK;
    if (want_ && ev != SG_EV_LIT)
      finish_clause ();
    switch (ev) {
    case SG_EV_ASSIGN: {
      const int kind = (int) ((pay >> 26) & 3u);
      const int level = (int) (pay & 0x1ffffffu);
      if (kind == SG_KIND_DECISION)
        decisions_++;
      else if (kind == SG_KIND_PROPAGATED)
        propagations_++;
      else
        inprocess_++;
      if (val && val <= (uint32_t) n_) {
        if (!assigned_[val])
          assigned_[val] = 1, trail_++;
        var_assigns_[val]++;
      }
      if (level > level_)
        level_ = level;
      max_level_ = std::max (max_level_, level_);
      break;
    }
    case SG_EV_UNASSIGN:
      unassigns_++;
      if (val && val <= (uint32_t) n_ && assigned_[val])
        assigned_[val] = 0, trail_--;
      break;
    case SG_EV_BACKTRACK:
      backtracks_++;
      level_ = (int) val;
      break;
    case SG_EV_GONE:
      if (pay & 1u)
        gone_++;
      else if (gone_)
        gone_--;
      break;
    case SG_EV_CONFLICT:
      conflicts_++;
      sum_conf_level_ += level_;
      sum_conf_trail_ += n_ ? (double) trail_ / (double) n_ : 0.0;
      if (ts_ && conflicts_ >= next_row_) {
        row ();
        next_row_ += ts_interval_;
      }
      break;
    case SG_EV_BACKJUMP: {
      backjumps_++;
      const int64_t d = (int64_t) pay - (int64_t) val;
      const uint64_t dist = d > 0 ? (uint64_t) d : 0;
      jump_dist_ += dist;
      if (dist <= 1)
        jump_chrono_++;
      size_t bin = 0;
      if (dist >= 1) {
        bin = 1;
        uint64_t x = dist - 1;
        while (x && bin + 1 < jump_hist_.size ())
          x >>= 1, bin++;
      }
      jump_hist_[bin]++;
      break;
    }
    case SG_EV_LEARN:
      cur_.clear ();
      want_ = pay ? pay : 1;
      cur_glue_ = val;
      break;
    case SG_EV_LIT:
      if (want_ && val && val <= (uint32_t) n_) {
        cur_.push_back (val);
        if (cur_.size () >= want_)
          finish_clause ();
      }
      break;
    case SG_EV_RESTART:
      restarts_++;
      break;
    case SG_EV_REDUCE:
      reductions_++;
      break;
    default:
      break;
    }
  }
}

// Gini coefficient of a list of non-negative counts (0: perfectly even,
// close to 1: concentrated on a few).
template <typename T> static double gini (std::vector<T> v) {
  std::sort (v.begin (), v.end ());
  double sum = 0, weighted = 0;
  const double n = (double) v.size ();
  for (size_t i = 0; i < v.size (); i++) {
    sum += (double) v[i];
    weighted += (double) (i + 1) * (double) v[i];
  }
  if (sum <= 0 || n < 2)
    return 0.0;
  return (2.0 * weighted) / (n * sum) - (n + 1.0) / n;
}

bool MetricsAnalyzer::write_json (const std::string &path, const std::string &extra, std::string &err) {
  if (want_)
    finish_clause ();
  if (ts_ && win_.clauses)
    row ();
  FILE *f = fopen (path.c_str (), "w");
  if (!f) {
    err = "cannot write '" + path + "'";
    return false;
  }
  auto ratio = [] (double a, double b) { return b > 0 ? a / b : 0.0; };
  const double L = learned_ ? (double) learned_ : 1.0;
  const double S = clauses_with_span_ ? (double) clauses_with_span_ : 1.0;
  fprintf (f, "{\n  \"variables\": %d,\n  \"clauses\": %zu", n_, cnf_.nclauses ());
  if (clu_)
    fprintf (f, ",\n  \"communities\": %d,\n  \"modularity\": %.4f", clu_->k, clu_->modularity);
  fprintf (f, "%s", extra.c_str ());
  fprintf (f, ",\n  \"events\": {\"decisions\": %llu, \"propagations\": %llu, \"inprocessing\": %llu, "
              "\"unassignments\": %llu, \"conflicts\": %llu, \"backjumps\": %llu, \"backtracks\": %llu, "
              "\"restarts\": %llu, \"reductions\": %llu, \"learned\": %llu, \"eliminated_at_end\": %llu}",
           (unsigned long long) decisions_, (unsigned long long) propagations_, (unsigned long long) inprocess_,
           (unsigned long long) unassigns_, (unsigned long long) conflicts_, (unsigned long long) backjumps_,
           (unsigned long long) backtracks_, (unsigned long long) restarts_, (unsigned long long) reductions_,
           (unsigned long long) learned_, (unsigned long long) gone_);
  fprintf (f, ",\n  \"search\": {\"max_level\": %d, \"mean_level_at_conflict\": %.3f, "
              "\"mean_trail_fraction_at_conflict\": %.4f}",
           max_level_, ratio (sum_conf_level_, (double) conflicts_), ratio (sum_conf_trail_, (double) conflicts_));
  fprintf (f, ",\n  \"backjumps\": {\"mean_distance\": %.3f, \"distance_at_most_1_fraction\": %.4f, "
              "\"histogram\": [",
           ratio ((double) jump_dist_, (double) backjumps_), ratio ((double) jump_chrono_, (double) backjumps_));
  size_t last = 0;
  for (size_t b = 0; b < jump_hist_.size (); b++)
    if (jump_hist_[b])
      last = b;
  for (size_t b = 0; b <= last; b++) {
    const uint64_t lo = b == 0 ? 0 : b == 1 ? 1 : (1ull << (b - 2)) + 1, hi = b == 0 ? 0 : b == 1 ? 1 : 1ull << (b - 1);
    fprintf (f, "%s[%llu, %llu, %llu]", b ? ", " : "", (unsigned long long) lo, (unsigned long long) hi,
             (unsigned long long) jump_hist_[b]);
  }
  fprintf (f, "]}");
  fprintf (f, ",\n  \"learned_clauses\": {\"mean_size\": %.3f, \"mean_glue\": %.3f", sum_size_ / L, sum_glue_ / L);
  if (clu_ && clauses_with_span_)
    fprintf (f, ", \"mean_span\": %.4f, \"expected_mean_span\": %.4f, \"locality\": %.4f, "
                "\"single_community_fraction\": %.4f, \"expected_single_community_fraction\": %.6f, "
                "\"focus_switch_rate\": %.4f",
             sum_span_ / S, sum_exp_span_ / S, ratio (sum_exp_span_, sum_span_), sum_single_ / S, sum_exp_single_ / S,
             ratio ((double) focus_switches_, (double) (clauses_with_span_ > 1 ? clauses_with_span_ - 1 : 1)));
  fprintf (f, "}");
  std::vector<uint32_t> va (var_assigns_.begin () + 1, var_assigns_.end ());
  std::vector<uint32_t> vl (var_learnt_.begin () + 1, var_learnt_.end ());
  fprintf (f, ",\n  \"activity\": {\"assignment_gini\": %.4f, \"learned_occurrence_gini\": %.4f", gini (va), gini (vl));
  if (clu_ && !com_learnt_.empty ()) {
    // share of learned-clause occurrences in the busiest 10% of communities
    std::vector<uint64_t> cl = com_learnt_;
    std::sort (cl.rbegin (), cl.rend ());
    uint64_t tot = 0, top = 0;
    const size_t k10 = std::max<size_t> (1, cl.size () / 10);
    for (size_t i = 0; i < cl.size (); i++) {
      tot += cl[i];
      if (i < k10)
        top += cl[i];
    }
    size_t touched = 0;
    for (uint64_t x : cl)
      if (x)
        touched++;
    fprintf (f, ", \"community_learned_gini\": %.4f, \"top10pct_communities_share\": %.4f, "
                "\"communities_touched_fraction\": %.4f",
             gini (com_learnt_), ratio ((double) top, (double) tot), ratio ((double) touched, (double) cl.size ()));
  }
  fprintf (f, "}\n}\n");
  fclose (f);
  return true;
}
