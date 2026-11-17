// Solver event traces.
//
// A trace is everything the hooks saw during one solver run, as 8-byte
// records (see SG_EV_* in sg_hooks.h), after a fixed header.  Traces can be
//  - written to disk (plain, or compressed when the name ends in .zst, .xz
//    or .gz; the matching command-line tool must be installed),
//  - analysed while the solver runs or afterwards (metrics.hpp),
//  - replayed in the visualizer exactly as the solver produced them.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cnf.hpp"

struct TraceHeader {
  char magic[8] = {'S', 'G', 'T', 'R', 'A', 'C', 'E', '1'};
  uint32_t version = 1;
  int32_t nvars = 0;
  uint64_t cnf_hash = 0; // cnf_fingerprint() of the formula
  char solver[64] = {};
  char cnf_name[128] = {};
};

// Order-sensitive 64-bit fingerprint of a formula (FNV-1a over all
// literals and clause ends).
uint64_t cnf_fingerprint (const Cnf &cnf);

// Receives event records (pairs of 32-bit words).
struct EventSink {
  virtual ~EventSink () {}
  virtual void consume (const uint32_t *words, size_t n) = 0;
};

// Hands every batch of events to several sinks.
struct EventFanout : EventSink {
  std::vector<EventSink *> sinks;
  void consume (const uint32_t *words, size_t n) override {
    for (EventSink *s : sinks)
      s->consume (words, n);
  }
};

class TraceWriter : public EventSink {
public:
  ~TraceWriter () override { close (); }
  bool open (const std::string &path, const TraceHeader &h, std::string &err);
  void consume (const uint32_t *words, size_t n) override;
  bool close (); // false if writing failed
  uint64_t bytes () const { return bytes_; }

private:
  FILE *f_ = nullptr;
  bool piped_ = false, failed_ = false;
  uint64_t bytes_ = 0;
};

class TraceReader {
public:
  ~TraceReader () { close (); }
  bool open (const std::string &path, TraceHeader &h, std::string &err);
  // Reads up to `max_words` words (always an even number); 0 at the end.
  size_t read (uint32_t *buf, size_t max_words);
  void close ();

private:
  FILE *f_ = nullptr;
  bool piped_ = false;
};

// Result and model recorded at the end of a trace.
struct TraceOutcome {
  int result = 0; // 10 SAT, 20 UNSAT, 0 unknown
  std::vector<uint32_t> model; // bit v-1 of word (v-1)/32 = value of v
};

// Feeds records through the hooks (sg_tls must point at the target
// context), and collects the outcome records.
void replay_events (const uint32_t *words, size_t n, TraceOutcome &out);

// Appends result and model records for a finished run.
void trace_outcome_records (int result, const std::vector<uint32_t> &state, int nvars,
                            std::vector<uint32_t> &words);
