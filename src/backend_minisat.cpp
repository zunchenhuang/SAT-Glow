// MiniSat backend (the SimpSolver, i.e. MiniSat with its preprocessor).
// The sources in third_party/minisat carry the hook calls, see
// patches/minisat-satglow.patch.
#include "minisat/simp/SimpSolver.h"
#include "solver.hpp"

namespace {

struct MinisatBackend : SolverBackend {
  Minisat::SimpSolver solver;

  MinisatBackend () { solver.verbosity = 0; }
  const char *name () const override { return "MiniSat 2.2"; }
  bool set_option (const std::string &n, int v) override {
    if (n == "elim")
      solver.use_elim = v != 0;
    else if (n == "asymm")
      solver.use_asymm = v != 0;
    else if (n == "rcheck")
      solver.use_rcheck = v != 0;
    else if (n == "luby")
      solver.luby_restart = v != 0;
    else if (n == "phase-saving" && v >= 0 && v <= 2)
      solver.phase_saving = v;
    else if (n == "ccmin-mode" && v >= 0 && v <= 2)
      solver.ccmin_mode = v;
    else if (n == "rfirst" && v > 0)
      solver.restart_first = v;
    else if (n == "rnd-init")
      solver.rnd_init_act = v != 0;
    else
      return false;
    return true;
  }
  void load (const Cnf &cnf) override {
    while (solver.nVars () < cnf.nvars)
      solver.newVar ();
    Minisat::vec<Minisat::Lit> clause;
    for (size_t i = 0; i < cnf.nclauses (); i++) {
      clause.clear ();
      for (const int *p = cnf.begin (i); p != cnf.end (i); p++)
        clause.push (Minisat::mkLit ((*p < 0 ? -*p : *p) - 1, *p < 0));
      solver.addClause_ (clause);
    }
  }
  int solve () override {
    Minisat::vec<Minisat::Lit> none;
    Minisat::lbool res = solver.solveLimited (none);
    return res == Minisat::l_True ? 10 : res == Minisat::l_False ? 20 : 0;
  }
  bool value (int var) override {
    return var <= solver.model.size () && solver.model[var - 1] == Minisat::l_True;
  }
  void terminate () override { solver.interrupt (); }
  void own_counts (uint64_t &c, uint64_t &d) override { c = solver.conflicts, d = solver.decisions; }
};

} // namespace

std::unique_ptr<SolverBackend> make_minisat_backend () {
  return std::unique_ptr<SolverBackend> (new MinisatBackend ());
}
