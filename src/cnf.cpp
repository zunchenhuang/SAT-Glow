#include "cnf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool ends_with (const std::string &s, const char *suf) {
  size_t n = strlen (suf);
  return s.size () >= n && !s.compare (s.size () - n, n, suf);
}

static std::string shell_quote (const std::string &s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'')
      r += "'\\''";
    else
      r += c;
  }
  return r + "'";
}

bool parse_dimacs (const std::string &path, Cnf &out, std::string &err) {
  FILE *f = nullptr;
  bool piped = false;
  const char *tool = nullptr;
  if (ends_with (path, ".xz"))
    tool = "xz -dc ";
  else if (ends_with (path, ".gz"))
    tool = "gzip -dc ";
  else if (ends_with (path, ".bz2"))
    tool = "bzip2 -dc ";
  if (tool) {
    f = popen ((std::string (tool) + shell_quote (path)).c_str (), "r");
    piped = true;
  } else
    f = fopen (path.c_str (), "rb");
  if (!f) {
    err = "cannot open '" + path + "'";
    return false;
  }

  std::vector<char> buf;
  {
    char tmp[1 << 16];
    size_t n;
    while ((n = fread (tmp, 1, sizeof tmp, f)) > 0)
      buf.insert (buf.end (), tmp, tmp + n);
  }
  if (piped)
    pclose (f);
  else
    fclose (f);
  buf.push_back ('\n');
  buf.push_back (0);

  out = Cnf ();
  size_t slash = path.find_last_of ('/');
  out.name = slash == std::string::npos ? path : path.substr (slash + 1);
  out.start.push_back (0);

  int header_vars = 0;
  int max_var = 0;
  bool open_clause = false;
  const char *p = buf.data ();
  const char *e = p + buf.size () - 1;
  while (p < e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
    if (p >= e)
      break;
    if (*p == 'c') { // comment
      while (p < e && *p != '\n')
        p++;
      continue;
    }
    if (*p == '%') // old SATLIB files end with "%\n0\n"
      break;
    if (*p == 'p') {
      long nc = 0;
      if (sscanf (p, "p cnf %d %ld", &header_vars, &nc) != 2) {
        err = "bad 'p cnf' header in '" + path + "'";
        return false;
      }
      if (nc > 0) {
        out.start.reserve ((size_t) nc + 1);
        out.lits.reserve ((size_t) nc * 3);
      }
      while (p < e && *p != '\n')
        p++;
      continue;
    }
    bool neg = false;
    if (*p == '-') {
      neg = true;
      p++;
    }
    if (*p < '0' || *p > '9') {
      err = "unexpected character in '" + path + "'";
      return false;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      if (v > 0x3fffffff) {
        err = "variable index too large in '" + path + "'";
        return false;
      }
      p++;
    }
    if (!v) {
      out.start.push_back ((uint32_t) out.lits.size ());
      open_clause = false;
    } else {
      if (v > max_var)
        max_var = (int) v;
      out.lits.push_back (neg ? -(int) v : (int) v);
      open_clause = true;
    }
  }
  if (open_clause) // last clause without terminating 0
    out.start.push_back ((uint32_t) out.lits.size ());
  out.nvars = header_vars > max_var ? header_vars : max_var;
  if (!out.nvars) {
    err = "no variables found in '" + path + "'";
    return false;
  }
  return true;
}
