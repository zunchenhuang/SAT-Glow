#!/usr/bin/env python3
"""Generate small DIMACS CNF instances to watch in satglow.

  gen_cnf.py random N [--ratio 4.26] [--k 3] [--seed S]   random k-SAT
  gen_cnf.py php HOLES                                    pigeonhole (UNSAT)
  gen_cnf.py queens N                                     N-queens (SAT for N>=4)
  gen_cnf.py latin N [--holes 0.42] [--seed S]            Latin square completion (SAT)
  gen_cnf.py factor NUMBER                                factor NUMBER with a multiplier circuit
  gen_cnf.py parity N [--seed S]                          chained XOR constraints (UNSAT, hard for CDCL)

Output goes to stdout unless -o FILE is given.
"""
import argparse
import random
import sys


class Cnf:
    def __init__(self):
        self.nvars = 0
        self.clauses = []
        self.comments = []

    def new(self):
        self.nvars += 1
        return self.nvars

    def add(self, *lits):
        self.clauses.append(list(lits))

    def write(self, out):
        for c in self.comments:
            out.write("c %s\n" % c)
        out.write("p cnf %d %d\n" % (self.nvars, len(self.clauses)))
        for c in self.clauses:
            out.write(" ".join(map(str, c)) + " 0\n")


def gen_random(n, ratio, k, seed):
    rng = random.Random(seed)
    f = Cnf()
    f.nvars = n
    f.comments.append("random %d-SAT, n=%d, ratio=%.3f, seed=%s" % (k, n, ratio, seed))
    for _ in range(int(round(n * ratio))):
        vs = rng.sample(range(1, n + 1), k)
        f.add(*[v if rng.random() < 0.5 else -v for v in vs])
    return f


def gen_php(holes):
    pigeons = holes + 1
    f = Cnf()
    f.comments.append("pigeonhole: %d pigeons into %d holes (unsatisfiable)" % (pigeons, holes))
    x = [[f.new() for _ in range(holes)] for _ in range(pigeons)]
    for p in range(pigeons):
        f.add(*x[p])
    for h in range(holes):
        for p in range(pigeons):
            for q in range(p + 1, pigeons):
                f.add(-x[p][h], -x[q][h])
    return f


def gen_queens(n):
    f = Cnf()
    f.comments.append("%d-queens" % n)
    x = [[f.new() for _ in range(n)] for _ in range(n)]
    for r in range(n):
        f.add(*x[r])
    for c in range(n):
        f.add(*[x[r][c] for r in range(n)])
    cells = [(r, c) for r in range(n) for c in range(n)]
    for i, (r1, c1) in enumerate(cells):
        for (r2, c2) in cells[i + 1:]:
            if r1 == r2 or c1 == c2 or abs(r1 - r2) == abs(c1 - c2):
                f.add(-x[r1][c1], -x[r2][c2])
    return f


def gen_latin(n, holes, seed):
    """Latin square completion (quasigroup with holes).  Always satisfiable."""
    rng = random.Random(seed)
    rows = list(range(n))
    cols = list(range(n))
    syms = list(range(n))
    rng.shuffle(rows)
    rng.shuffle(cols)
    rng.shuffle(syms)
    square = [[syms[(rows[r] + cols[c]) % n] for c in range(n)] for r in range(n)]
    cells = [(r, c) for r in range(n) for c in range(n)]
    rng.shuffle(cells)
    open_cells = sorted(cells[:int(round(holes * n * n))])
    is_open = set(open_cells)
    f = Cnf()
    f.comments.append("latin square completion: order %d, %d holes (%.0f%%), seed=%s"
                      % (n, len(open_cells), 100.0 * len(open_cells) / (n * n), seed))
    row_used = [set(square[r][c] for c in range(n) if (r, c) not in is_open) for r in range(n)]
    col_used = [set(square[r][c] for r in range(n) if (r, c) not in is_open) for c in range(n)]
    var = {}
    for (r, c) in open_cells:
        for v in range(n):
            if v not in row_used[r] and v not in col_used[c]:
                var[(r, c, v)] = f.new()

    def exactly_one(lits):
        f.add(*lits)
        for i in range(len(lits)):
            for j in range(i + 1, len(lits)):
                f.add(-lits[i], -lits[j])

    for (r, c) in open_cells:
        exactly_one([var[(r, c, v)] for v in range(n) if (r, c, v) in var])
    for r in range(n):
        for v in range(n):
            if v not in row_used[r]:
                exactly_one([var[(r, c, v)] for c in range(n) if (r, c, v) in var])
    for c in range(n):
        for v in range(n):
            if v not in col_used[c]:
                exactly_one([var[(r, c, v)] for r in range(n) if (r, c, v) in var])
    return f


def gate_and(f, a, b):
    o = f.new()
    f.add(-a, -b, o)
    f.add(a, -o)
    f.add(b, -o)
    return o


def gate_xor(f, a, b):
    o = f.new()
    f.add(-a, -b, -o)
    f.add(a, b, -o)
    f.add(a, -b, o)
    f.add(-a, b, o)
    return o


def gate_or(f, a, b):
    o = f.new()
    f.add(a, b, -o)
    f.add(-a, o)
    f.add(-b, o)
    return o


def full_adder(f, a, b, c):
    t = gate_xor(f, a, b)
    s = gate_xor(f, t, c)
    carry = gate_or(f, gate_and(f, a, b), gate_and(f, t, c))
    return s, carry


def gen_factor(number):
    """p * q = number with 1 < p <= q, as an array multiplier circuit."""
    nbits = number.bit_length()
    pb = (nbits + 1) // 2  # smaller factor has at most half the bits
    qb = nbits - 1
    f = Cnf()
    f.comments.append("factor %d: %d-bit x %d-bit array multiplier" % (number, pb, qb))
    p = [f.new() for _ in range(pb)]
    q = [f.new() for _ in range(qb)]
    # exclude the trivial factor 1
    f.add(*p[1:])
    f.add(*q[1:])
    width = pb + qb
    acc = [None] * width  # None means constant 0
    for i in range(pb):
        row = [None] * width
        for j in range(qb):
            row[i + j] = gate_and(f, p[i], q[j])
        carry = None
        nxt = [None] * width
        for k in range(width):
            a, b, c = acc[k], row[k], carry
            ins = [v for v in (a, b, c) if v is not None]
            if len(ins) == 0:
                nxt[k], carry = None, None
            elif len(ins) == 1:
                nxt[k], carry = ins[0], None
            elif len(ins) == 2:
                nxt[k] = gate_xor(f, ins[0], ins[1])
                carry = gate_and(f, ins[0], ins[1])
            else:
                nxt[k], carry = full_adder(f, a, b, c)
        acc = nxt
    for k in range(width):
        bit = (number >> k) & 1
        if acc[k] is None:
            if bit:
                f.add()  # impossible, makes the formula unsatisfiable
        else:
            f.add(acc[k] if bit else -acc[k])
    return f


def gen_parity(n, seed):
    """Two XOR chains over the same variables with opposite parity."""
    rng = random.Random(seed)
    f = Cnf()
    f.comments.append("parity: two XOR chains over %d variables with opposite parity (unsatisfiable), seed=%s" % (n, seed))
    xs = [f.new() for _ in range(n)]

    def chain(order, parity):
        acc = order[0]
        for v in order[1:]:
            acc = gate_xor(f, acc, v)
        f.add(acc if parity else -acc)

    chain(xs, 1)
    perm = xs[:]
    rng.shuffle(perm)
    chain(perm, 0)
    return f


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-o", "--output", help="write to FILE instead of stdout")
    sub = ap.add_subparsers(dest="kind", required=True)
    a = sub.add_parser("random", parents=[common])
    a.add_argument("n", type=int)
    a.add_argument("--ratio", type=float, default=4.26)
    a.add_argument("--k", type=int, default=3)
    a.add_argument("--seed", type=int, default=1)
    a = sub.add_parser("php", parents=[common])
    a.add_argument("holes", type=int)
    a = sub.add_parser("queens", parents=[common])
    a.add_argument("n", type=int)
    a = sub.add_parser("latin", parents=[common])
    a.add_argument("n", type=int)
    a.add_argument("--holes", type=float, default=0.42)
    a.add_argument("--seed", type=int, default=1)
    a = sub.add_parser("factor", parents=[common])
    a.add_argument("number", type=int)
    a = sub.add_parser("parity", parents=[common])
    a.add_argument("n", type=int)
    a.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    if args.kind == "random":
        f = gen_random(args.n, args.ratio, args.k, args.seed)
    elif args.kind == "php":
        f = gen_php(args.holes)
    elif args.kind == "queens":
        f = gen_queens(args.n)
    elif args.kind == "latin":
        f = gen_latin(args.n, args.holes, args.seed)
    elif args.kind == "factor":
        f = gen_factor(args.number)
    else:
        f = gen_parity(args.n, args.seed)

    if args.output:
        with open(args.output, "w") as out:
            f.write(out)
    else:
        f.write(sys.stdout)


if __name__ == "__main__":
    main()
