// CaDiCaL backend.  The solver sources in third_party/cadical carry the
// hook calls (see patches/cadical-2.1.3-satglow.patch); nothing else about
// the solver is changed.
#include "cadical.hpp"
#include "solver.hpp"

namespace {

struct CadicalBackend : SolverBackend {
  CaDiCaL::Solver solver;
  std::string label;

  CadicalBackend () {
    label = std::string ("CaDiCaL ") + CaDiCaL::Solver::version ();
    solver.set ("quiet", 1);
  }
  const char *name () const override { return label.c_str (); }
  bool set_option (const std::string &n, int v) override {
    return solver.set (n.c_str (), v);
  }
  void load (const Cnf &cnf) override {
    solver.reserve (cnf.nvars);
    for (size_t i = 0; i < cnf.nclauses (); i++) {
      for (const int *p = cnf.begin (i); p != cnf.end (i); p++)
        solver.add (*p);
      solver.add (0);
    }
  }
  int solve () override { return solver.solve (); }
  bool value (int var) override { return solver.val (var) > 0; }
  void terminate () override { solver.terminate (); }
  void own_counts (uint64_t &c, uint64_t &d) override {
    int64_t x = 0, y = 0;
    solver.satglow_counts (x, y);
    c = (uint64_t) x, d = (uint64_t) y;
  }
};

} // namespace

std::unique_ptr<SolverBackend> make_cadical_backend () {
  return std::unique_ptr<SolverBackend> (new CadicalBackend ());
}
