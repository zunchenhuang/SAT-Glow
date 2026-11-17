/* satglow solver hooks.
 *
 * This is the only thing a SAT solver has to know about the visualizer.
 * It is plain C so that it can be included from C solvers (Kissat) as well
 * as C++ ones (CaDiCaL, MiniSat).
 *
 * The solver thread sets the thread-local `sg_tls` to its context before it
 * starts and then calls the inline functions below from its hot paths.  Each
 * hook is a handful of plain memory writes; there is no locking and no
 * queue.  The render thread reads the same memory once per frame.
 *
 * All variables are EXTERNAL (DIMACS) indices in 1..nvars.
 *
 * Two optional extras:
 *  - Tracing: if `trace_buf` is set, every hook also appends one 8-byte
 *    record to it (see SG_EV_*); sg_trace_flush() empties the buffer.  The
 *    same records can be replayed through the hooks later.
 *  - Compiling a solver with -DSG_DISABLE_HOOKS turns every hook into
 *    nothing, which gives the baseline for overhead measurements.
 */
#ifndef SG_HOOKS_H
#define SG_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packed per-variable state word:
 *   bits 0-1 : 0 unassigned, 1 true, 2 false
 *   bits 2-4 : kind of assignment (SG_KIND_*)
 *   bit  5   : variable was removed from the search by preprocessing
 *              (eliminated, substituted or pure); it stays unassigned
 *   bits 8-31: decision level                                              */
#define SG_VAL_MASK 3u
#define SG_VAL_TRUE 1u
#define SG_VAL_FALSE 2u
#define SG_KIND_SHIFT 2
#define SG_KIND_MASK 7u
#define SG_GONE 0x20u
#define SG_LEVEL_SHIFT 8

/* How many variables of the latest learned clause are kept (shockwaves). */
#define SG_CLAUSE_MAX 64

enum {
  SG_KIND_PROPAGATED = 0, /* unit propagation during search          */
  SG_KIND_DECISION = 1,   /* branching decision                      */
  SG_KIND_INPROCESS = 2,  /* probing / vivification / instantiation  */
  SG_KIND_MODEL = 3       /* filled in from the final model          */
};

typedef struct sg_ctx {
  /* per-variable arrays, index 0 unused */
  uint32_t *state;  /* packed state word, see above                       */
  uint32_t *flips;  /* how often the variable has been assigned so far    */
  uint32_t *learnt; /* how often it occurred in a learned clause          */
  int32_t nvars;

  /* speed throttle: every assign/unassign costs one token */
  int64_t budget;

  /* counters (written by the solver thread only) */
  uint64_t assigns;       /* all assignments                              */
  uint64_t decisions;
  uint64_t propagations;  /* search propagations                          */
  uint64_t inprocessing;  /* assignments made by inprocessing             */
  uint64_t unassigns;
  uint64_t conflicts;
  uint64_t backjumps;     /* conflict-driven backtracks                   */
  uint64_t backjump_dist; /* sum of (level - new_level) over backjumps    */
  uint64_t restarts;
  uint64_t reductions;    /* learned-clause database reductions           */
  uint64_t learned;       /* learned clauses                              */
  uint64_t learned_lits;  /* sum of their sizes                           */
  uint64_t learned_glue;  /* sum of their glue (LBD)                      */
  int64_t trail;          /* currently assigned variables                 */
  int64_t fixed;          /* assigned at level 0                          */
  int64_t gone;           /* removed by preprocessing (eliminated etc.)   */
  int32_t level;          /* current decision level                       */
  int32_t max_level;      /* deepest level seen                           */
  int32_t last_jump_from; /* last backjump: from level ...                */
  int32_t last_jump_to;   /* ... to level                                 */

  /* The most recent learned clause (first SG_CLAUSE_MAX variables).  The
   * render thread may read it while it is being overwritten; that only
   * affects which variables a shockwave starts from. */
  uint64_t clause_seq;                /* bumped for every learned clause  */
  int32_t clause_len;
  int32_t clause_vars[SG_CLAUSE_MAX];

  /* Optional event trace, two 32-bit words per event. */
  uint32_t *trace_buf;
  uint32_t trace_len, trace_cap;
  void *trace_user;

  void *user; /* owned by the application */
} sg_ctx;

extern __thread sg_ctx *sg_tls;

#ifdef SG_DISABLE_HOOKS
#define SG_CTX() ((sg_ctx *) 0) /* every hook folds away to nothing */
#else
#define SG_CTX() sg_tls
#endif

/* Trace records: head word = event << 28 | 28 bits of payload, then one
 * value word. */
enum {
  SG_EV_ASSIGN = 1,    /* head: kind << 26 | negative << 25 | level; value: var */
  SG_EV_UNASSIGN = 2,  /* value: var */
  SG_EV_BACKTRACK = 3, /* value: new level */
  SG_EV_GONE = 4,      /* head: gone flag; value: var */
  SG_EV_CONFLICT = 5,
  SG_EV_BACKJUMP = 6,  /* head: from level; value: to level */
  SG_EV_LEARN = 7,     /* head: size; value: glue */
  SG_EV_LIT = 8,       /* head: negative; value: var (follows SG_EV_LEARN) */
  SG_EV_RESTART = 9,
  SG_EV_REDUCE = 10,
  SG_EV_RESULT = 14,   /* written by the application after solving */
  SG_EV_MODEL = 15     /* written by the application: 32 model bits */
};
#define SG_PAYLOAD_MASK 0x0fffffffu

/* Empties the trace buffer, implemented by the application. */
void sg_trace_flush (sg_ctx *ctx);

static inline void sg_trace (sg_ctx *c, uint32_t head, uint32_t value) {
  c->trace_buf[c->trace_len++] = head;
  c->trace_buf[c->trace_len++] = value;
  if (c->trace_len + 2 > c->trace_cap)
    sg_trace_flush (c);
}

static inline uint32_t sg_clamp28 (int64_t x) {
  return x < 0 ? 0u : x > (int64_t) SG_PAYLOAD_MASK ? SG_PAYLOAD_MASK : (uint32_t) x;
}

/* Slow path of the throttle, implemented by the application.  Blocks until
 * the solver may continue and refills ctx->budget. */
void sg_wait (sg_ctx *ctx);

#define SG_STORE(p, v) __atomic_store_n ((p), (v), __ATOMIC_RELAXED)
#define SG_LOAD(p) __atomic_load_n ((p), __ATOMIC_RELAXED)
#define SG_BUMP(p) SG_STORE ((p), *(p) + 1)

static inline void sg_tick (sg_ctx *c) {
  if (--c->budget < 0)
    sg_wait (c);
}

/* Variable becomes assigned.  `elit` is a signed external literal. */
static inline void sg_assign (int elit, int level, int kind) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_ASSIGN << 28 | (uint32_t) (kind & 3) << 26 |
                    (uint32_t) (elit < 0) << 25 | (sg_clamp28 (level) & 0x1ffffffu),
              (uint32_t) (elit < 0 ? -elit : elit));
  int v = elit < 0 ? -elit : elit;
  if (!v || v > c->nvars)
    return;
  uint32_t w = (elit > 0 ? SG_VAL_TRUE : SG_VAL_FALSE) |
               ((uint32_t) kind << SG_KIND_SHIFT) |
               ((uint32_t) level << SG_LEVEL_SHIFT) |
               (c->state[v] & SG_GONE);
  SG_STORE (&c->state[v], w);
  SG_BUMP (&c->flips[v]);
  SG_BUMP (&c->assigns);
  SG_STORE (&c->trail, c->trail + 1);
  if (!level)
    SG_STORE (&c->fixed, c->fixed + 1);
  if (level > c->level) { /* a new decision level was opened */
    SG_STORE (&c->level, level);
    if (level > c->max_level)
      SG_STORE (&c->max_level, level);
  }
  if (kind == SG_KIND_DECISION)
    SG_BUMP (&c->decisions);
  else if (kind == SG_KIND_PROPAGATED)
    SG_BUMP (&c->propagations);
  else
    SG_BUMP (&c->inprocessing);
  sg_tick (c);
}

/* Variable becomes unassigned (backtracking). */
static inline void sg_unassign (int elit) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_UNASSIGN << 28, (uint32_t) (elit < 0 ? -elit : elit));
  int v = elit < 0 ? -elit : elit;
  if (!v || v > c->nvars)
    return;
  uint32_t w = c->state[v];
  if (!(w & SG_VAL_MASK))
    return;
  if (!(w >> SG_LEVEL_SHIFT))
    SG_STORE (&c->fixed, c->fixed - 1);
  SG_STORE (&c->state[v], w & SG_GONE);
  SG_BUMP (&c->unassigns);
  SG_STORE (&c->trail, c->trail - 1);
  sg_tick (c);
}

/* The solver moved to decision level `new_level` (any kind of backtrack). */
static inline void sg_backtrack (int new_level) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_BACKTRACK << 28, (uint32_t) new_level);
  SG_STORE (&c->level, new_level);
}

/* Preprocessing removed the variable from the search (gone = 1), or put it
 * back (gone = 0). */
static inline void sg_gone (int elit, int gone) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_GONE << 28 | (gone ? 1u : 0u), (uint32_t) (elit < 0 ? -elit : elit));
  int v = elit < 0 ? -elit : elit;
  if (!v || v > c->nvars)
    return;
  uint32_t w = c->state[v];
  if (gone && !(w & SG_GONE)) {
    SG_STORE (&c->state[v], w | SG_GONE);
    SG_STORE (&c->gone, c->gone + 1);
  } else if (!gone && (w & SG_GONE)) {
    SG_STORE (&c->state[v], w & ~SG_GONE);
    SG_STORE (&c->gone, c->gone - 1);
  }
}

static inline void sg_conflict (void) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_CONFLICT << 28, 0u);
  SG_BUMP (&c->conflicts);
}

/* Conflict analysis decided to jump from `from_level` back to `to_level`. */
static inline void sg_backjump (int from_level, int to_level) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_BACKJUMP << 28 | sg_clamp28 (from_level), (uint32_t) to_level);
  SG_BUMP (&c->backjumps);
  SG_STORE (&c->backjump_dist,
            c->backjump_dist + (uint64_t) (from_level - to_level));
  SG_STORE (&c->last_jump_from, from_level);
  SG_STORE (&c->last_jump_to, to_level);
}

/* A clause was learned.  Call sg_learned_lit for each of its literals. */
static inline void sg_learned (int size, int glue) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_LEARN << 28 | sg_clamp28 (size), (uint32_t) glue);
  SG_BUMP (&c->learned);
  SG_STORE (&c->learned_lits, c->learned_lits + (uint64_t) size);
  SG_STORE (&c->learned_glue, c->learned_glue + (uint64_t) glue);
  SG_STORE (&c->clause_len, 0);
  SG_BUMP (&c->clause_seq);
}

static inline void sg_learned_lit (int elit) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_LIT << 28 | (uint32_t) (elit < 0), (uint32_t) (elit < 0 ? -elit : elit));
  int v = elit < 0 ? -elit : elit;
  if (!v || v > c->nvars)
    return;
  SG_BUMP (&c->learnt[v]);
  int32_t k = c->clause_len;
  if (k < SG_CLAUSE_MAX) {
    SG_STORE (&c->clause_vars[k], v);
    SG_STORE (&c->clause_len, k + 1);
  }
}

static inline void sg_restart (void) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_RESTART << 28, 0u);
  SG_BUMP (&c->restarts);
}

static inline void sg_reduce (void) {
  sg_ctx *c = SG_CTX ();
  if (!c)
    return;
  if (c->trace_buf)
    sg_trace (c, (uint32_t) SG_EV_REDUCE << 28, 0u);
  SG_BUMP (&c->reductions);
}

#ifdef __cplusplus
}
#endif

#endif
