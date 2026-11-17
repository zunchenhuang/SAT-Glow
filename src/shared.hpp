// State shared between the solver thread and the render thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "sg_hooks.h"
#include "trace.hpp"

enum SolveStatus {
  ST_IDLE = 0,
  ST_LOADING = 1,
  ST_RUNNING = 2,
  ST_ABORTED = 3,
  ST_SAT = 10,
  ST_UNSAT = 20
};

struct Shared {
  sg_ctx ctx{};
  std::vector<uint32_t> state, flips, learnt;

  // Speed control.  The render thread drops tokens into `tokens`; the solver
  // thread spends one per assignment / unassignment.
  std::atomic<int64_t> tokens{0};
  std::atomic<bool> unlimited{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> quit{false};

  std::atomic<int> status{ST_IDLE};
  std::atomic<bool> model_ok{false};
  std::atomic<int64_t> hook_errors{0}; // must stay 0, see solver.cpp
  std::atomic<int64_t> model_repaired{0};
  std::atomic<double> solve_seconds{0.0};

  // Tracing: if `trace_sink` is set, reset() attaches a trace buffer to the
  // context and full buffers are handed to the sink (in the solver thread).
  EventSink *trace_sink = nullptr;
  std::vector<uint32_t> trace_storage;

  void reset (int nvars);
  void flush_trace (); // hands what is left in the buffer to the sink
};
