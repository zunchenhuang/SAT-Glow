// Solver backends and the thread that runs one.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cnf.hpp"
#include "shared.hpp"

// A SAT solver that has been patched to call the hooks in sg_hooks.h.
struct SolverBackend {
  virtual ~SolverBackend () {}
  virtual const char *name () const = 0;
  virtual bool set_option (const std::string &name, int value) = 0;
  virtual void load (const Cnf &cnf) = 0;
  virtual int solve () = 0;        // 10 = SAT, 20 = UNSAT, 0 = interrupted
  virtual bool value (int var) = 0; // model value, only valid after SAT
  virtual void terminate () = 0;    // may be called from another thread
  // The solver's own conflict and decision counters (not the hooks').
  virtual void own_counts (uint64_t &conflicts, uint64_t &decisions) = 0;
};

std::unique_ptr<SolverBackend> make_cadical_backend ();
std::unique_ptr<SolverBackend> make_minisat_backend ();
std::unique_ptr<SolverBackend> make_kissat_backend ();

// The solvers that can be selected by name.
static const char *const SOLVER_NAMES[] = {"cadical", "kissat", "minisat"};
static const int NUM_SOLVERS = 3;
inline int solver_index (const std::string &name) {
  for (int i = 0; i < NUM_SOLVERS; i++)
    if (name == SOLVER_NAMES[i])
      return i;
  return -1;
}
std::unique_ptr<SolverBackend> make_backend (int solver);

// Backend 3 replays a recorded trace instead of solving.
static const int REPLAY_SOLVER = 3;
std::unique_ptr<SolverBackend> make_replay_backend (const std::string &path);
void set_replay_path (const std::string &path);
const std::string &replay_path ();

// Runs one backend on one formula in its own thread.
class SolverRunner {
public:
  ~SolverRunner () { stop (); }
  void start (const Cnf &cnf, Shared &shared, int solver,
              const std::vector<std::pair<std::string, int>> &options);
  void stop ();
  std::string solver_name;
  bool hooks = true; // false: run the solver with no hook context at all
  uint64_t own_conflicts = 0, own_decisions = 0; // after the run

private:
  std::thread thread_;
  std::atomic<bool> done_{true};
  std::unique_ptr<SolverBackend> backend_;
  Shared *shared_ = nullptr;
};
