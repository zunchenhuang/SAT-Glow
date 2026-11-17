#include "solver.hpp"

#include <chrono>
#include <thread>
#include <cstdio>

static std::string g_replay_path;
void set_replay_path (const std::string &path) { g_replay_path = path; }
const std::string &replay_path () { return g_replay_path; }

std::unique_ptr<SolverBackend> make_backend (int solver) {
  switch (solver) {
  case REPLAY_SOLVER:
    return make_replay_backend (g_replay_path);
  case 1:
    return make_kissat_backend ();
  case 2:
    return make_minisat_backend ();
  default:
    return make_cadical_backend ();
  }
}

void SolverRunner::start (
    const Cnf &cnf, Shared &shared, int solver,
    const std::vector<std::pair<std::string, int>> &options) {
  stop ();
  shared_ = &shared;
  shared.reset (cnf.nvars);
  shared.quit.store (false);
  backend_ = make_backend (solver);
  solver_name = backend_->name ();
  for (const auto &o : options)
    if (!backend_->set_option (o.first, o.second))
      fprintf (stderr, "satglow: solver rejected option %s=%d\n",
               o.first.c_str (), o.second);
  shared.status.store (ST_LOADING);

  SolverBackend *be = backend_.get ();
  const Cnf *formula = &cnf;
  Shared *sh = &shared;
  done_.store (false);
  std::atomic<bool> *done = &done_;
  const bool hooks = this->hooks;
  uint64_t *own_c = &own_conflicts, *own_d = &own_decisions;
  thread_ = std::thread ([be, formula, sh, done, hooks, own_c, own_d] {
    sg_tls = hooks ? &sh->ctx : nullptr;
    be->load (*formula);
    sh->status.store (ST_RUNNING);
    auto t0 = std::chrono::steady_clock::now ();
    int res = be->solve ();
    auto t1 = std::chrono::steady_clock::now ();
    sh->solve_seconds.store (std::chrono::duration<double> (t1 - t0).count ());
    sg_tls = nullptr;
    be->own_counts (*own_c, *own_d);

    // Integrity check of the hooks: the trail counter must match the number
    // of variables that are lit.
    {
      int64_t lit = 0;
      for (int v = 1; v <= formula->nvars; v++)
        if (SG_LOAD (&sh->state[v]) & SG_VAL_MASK)
          lit++;
      sh->hook_errors.store (lit != SG_LOAD (&sh->ctx.trail));
    }

    if (res == 10) {
      // Check the model against the original formula, then light up the
      // variables the solver never put on its trail (e.g. eliminated ones).
      bool ok = true;
      for (size_t i = 0; ok && i < formula->nclauses (); i++) {
        bool sat = false;
        for (const int *p = formula->begin (i); p != formula->end (i); p++)
          if (be->value (*p < 0 ? -*p : *p) == (*p > 0)) {
            sat = true;
            break;
          }
        if (!sat && formula->begin (i) != formula->end (i))
          ok = false;
        if (formula->begin (i) == formula->end (i))
          ok = false;
      }
      sh->model_ok.store (ok);
      // The final model can also differ from the trail on a few variables:
      // the solver repairs them when it undoes variable elimination.
      int64_t repaired = 0;
      for (int v = 1; v <= formula->nvars; v++) {
        uint32_t old = SG_LOAD (&sh->state[v]) & SG_VAL_MASK;
        uint32_t val = be->value (v) ? SG_VAL_TRUE : SG_VAL_FALSE;
        if (old == val)
          continue;
        if (old)
          repaired++;
        SG_STORE (&sh->state[v],
                  val | ((uint32_t) SG_KIND_MODEL << SG_KIND_SHIFT) |
                      (SG_LOAD (&sh->state[v]) & SG_GONE));
      }
      sh->model_repaired.store (repaired);
      sh->status.store (ST_SAT);
    } else if (res == 20)
      sh->status.store (ST_UNSAT);
    else
      sh->status.store (ST_ABORTED);
    done->store (true);
  });
}

void SolverRunner::stop () {
  if (thread_.joinable ()) {
    shared_->quit.store (true);
    // Ask again until the solver has really stopped: a request that arrives
    // while the formula is still loading may be dropped by the solver.
    while (!done_.load ()) {
      backend_->terminate ();
      std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    thread_.join ();
    shared_->quit.store (false);
  }
  backend_.reset ();
}
