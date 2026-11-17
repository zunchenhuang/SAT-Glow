#include "shared.hpp"

#include <chrono>
#include <cstring>
#include <thread>

__thread sg_ctx *sg_tls = nullptr;

void Shared::reset (int nvars) {
  state.assign ((size_t) nvars + 1, 0);
  flips.assign ((size_t) nvars + 1, 0);
  learnt.assign ((size_t) nvars + 1, 0);
  ctx = sg_ctx{};
  ctx.state = state.data ();
  ctx.flips = flips.data ();
  ctx.learnt = learnt.data ();
  ctx.nvars = nvars;
  ctx.budget = 0;
  ctx.user = this;
  if (trace_sink) {
    trace_storage.resize (1u << 20);
    ctx.trace_buf = trace_storage.data ();
    ctx.trace_cap = (uint32_t) trace_storage.size ();
    ctx.trace_len = 0;
    ctx.trace_user = trace_sink;
  }
  tokens.store (0);
  model_ok.store (false);
  hook_errors.store (0);
  model_repaired.store (0);
  solve_seconds.store (0.0);
}

void Shared::flush_trace () {
  if (ctx.trace_buf && ctx.trace_len)
    sg_trace_flush (&ctx);
}

extern "C" void sg_trace_flush (sg_ctx *c) {
  if (c->trace_user)
    ((EventSink *) c->trace_user)->consume (c->trace_buf, c->trace_len);
  c->trace_len = 0;
}

// Slow path of the throttle.  Called by the solver thread whenever it has
// used up its local budget.
extern "C" void sg_wait (sg_ctx *c) {
  Shared *s = (Shared *) c->user;
  for (;;) {
    if (s->quit.load (std::memory_order_relaxed)) {
      c->budget = INT64_MAX / 2; // run free, the solver is being terminated
      return;
    }
    if (!s->paused.load (std::memory_order_relaxed)) {
      if (s->unlimited.load (std::memory_order_relaxed)) {
        c->budget = 4095; // re-check the controls every 4096 events
        return;
      }
    }
    // Paused solvers still accept single-step tokens.
    int64_t t = s->tokens.exchange (0, std::memory_order_relaxed);
    if (t > 0) {
      c->budget = t - 1;
      return;
    }
    std::this_thread::sleep_for (std::chrono::microseconds (300));
  }
}
