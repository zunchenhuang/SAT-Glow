// Kissat backend.  The sources in third_party/kissat carry the hook calls,
// see patches/kissat-4.0.4-satglow.patch.
#include "solver.hpp"

extern "C" {
#include "kissat.h"
// From Kissat's options.h: non-null if an option of that name exists.
const void *kissat_options_has (const char *name);
// satglow.c in the bundled Kissat
void kissat_satglow_counts (kissat *solver, uint64_t *conflicts, uint64_t *decisions);
}

namespace {

struct KissatBackend : SolverBackend {
  kissat *solver;
  std::string label;

  KissatBackend () : solver (kissat_init ()) {
    label = std::string ("Kissat ") + kissat_version ();
  }
  ~KissatBackend () override { kissat_release (solver); }
  const char *name () const override { return label.c_str (); }
  bool set_option (const std::string &n, int v) override {
    if (!kissat_options_has (n.c_str ()))
      return false;
    kissat_set_option (solver, n.c_str (), v);
    return true;
  }
  void load (const Cnf &cnf) override {
    kissat_reserve (solver, cnf.nvars);
    for (size_t i = 0; i < cnf.nclauses (); i++) {
      for (const int *p = cnf.begin (i); p != cnf.end (i); p++)
        kissat_add (solver, *p);
      kissat_add (solver, 0);
    }
  }
  int solve () override { return kissat_solve (solver); }
  bool value (int var) override { return kissat_value (solver, var) > 0; }
  void terminate () override { kissat_terminate (solver); }
  void own_counts (uint64_t &c, uint64_t &d) override { kissat_satglow_counts (solver, &c, &d); }
};

} // namespace

std::unique_ptr<SolverBackend> make_kissat_backend () {
  return std::unique_ptr<SolverBackend> (new KissatBackend ());
}
