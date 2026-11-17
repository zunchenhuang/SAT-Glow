#include "trace.hpp"

#include <cstring>

#include "sg_hooks.h"

static bool ends_with (const std::string &s, const char *suf) {
  const size_t n = strlen (suf);
  return s.size () >= n && !s.compare (s.size () - n, n, suf);
}

static std::string quote (const std::string &s) {
  std::string r = "'";
  for (char c : s)
    r += c == '\'' ? std::string ("'\\''") : std::string (1, c);
  return r + "'";
}

static const char *compressor (const std::string &path, bool decompress) {
  if (ends_with (path, ".zst"))
    return decompress ? "zstd -q -d -c " : "zstd -q -3 -T0 -c > ";
  if (ends_with (path, ".xz"))
    return decompress ? "xz -d -c " : "xz -1 -T0 -c > ";
  if (ends_with (path, ".gz"))
    return decompress ? "gzip -d -c " : "gzip -1 -c > ";
  return nullptr;
}

uint64_t cnf_fingerprint (const Cnf &cnf) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h] (uint32_t x) {
    for (int i = 0; i < 4; i++) {
      h ^= (x >> (8 * i)) & 0xffu;
      h *= 1099511628211ull;
    }
  };
  mix ((uint32_t) cnf.nvars);
  for (size_t i = 0; i < cnf.nclauses (); i++) {
    for (const int *p = cnf.begin (i); p != cnf.end (i); p++)
      mix ((uint32_t) *p);
    mix (0);
  }
  return h;
}

bool TraceWriter::open (const std::string &path, const TraceHeader &h, std::string &err) {
  close ();
  const char *tool = compressor (path, false);
  if (tool) {
    f_ = popen ((std::string (tool) + quote (path)).c_str (), "w");
    piped_ = true;
  } else {
    f_ = fopen (path.c_str (), "wb");
    piped_ = false;
  }
  if (!f_) {
    err = "cannot write trace '" + path + "'";
    return false;
  }
  failed_ = false;
  bytes_ = 0;
  if (fwrite (&h, sizeof h, 1, f_) != 1)
    failed_ = true;
  bytes_ += sizeof h;
  return true;
}

void TraceWriter::consume (const uint32_t *words, size_t n) {
  if (!f_ || failed_ || !n)
    return;
  if (fwrite (words, sizeof (uint32_t), n, f_) != n)
    failed_ = true;
  bytes_ += n * sizeof (uint32_t);
}

bool TraceWriter::close () {
  if (!f_)
    return !failed_;
  const int rc = piped_ ? pclose (f_) : fclose (f_);
  f_ = nullptr;
  if (rc != 0)
    failed_ = true;
  return !failed_;
}

bool TraceReader::open (const std::string &path, TraceHeader &h, std::string &err) {
  close ();
  const char *tool = compressor (path, true);
  if (tool) {
    // stderr is silenced: closing a replay early breaks the pipe on purpose
    f_ = popen ((std::string (tool) + quote (path) + " 2>/dev/null").c_str (), "r");
    piped_ = true;
  } else {
    f_ = fopen (path.c_str (), "rb");
    piped_ = false;
  }
  if (!f_) {
    err = "cannot read trace '" + path + "'";
    return false;
  }
  TraceHeader want;
  if (fread (&h, sizeof h, 1, f_) != 1 || memcmp (h.magic, want.magic, sizeof h.magic) || h.version != 1) {
    err = "'" + path + "' is not a SATGlow trace";
    close ();
    return false;
  }
  h.solver[sizeof h.solver - 1] = 0;
  h.cnf_name[sizeof h.cnf_name - 1] = 0;
  return true;
}

size_t TraceReader::read (uint32_t *buf, size_t max_words) {
  if (!f_)
    return 0;
  size_t n = fread (buf, sizeof (uint32_t), max_words & ~(size_t) 1, f_);
  return n & ~(size_t) 1;
}

void TraceReader::close () {
  if (!f_)
    return;
  if (piped_)
    pclose (f_);
  else
    fclose (f_);
  f_ = nullptr;
}

void replay_events (const uint32_t *w, size_t n, TraceOutcome &out) {
  for (size_t i = 0; i + 1 < n; i += 2) {
    const uint32_t head = w[i], val = w[i + 1];
    const uint32_t pay = head & SG_PAYLOAD_MASK;
    switch (head >> 28) {
    case SG_EV_ASSIGN: {
      const int var = (int) val;
      sg_assign ((pay >> 25) & 1u ? -var : var, (int) (pay & 0x1ffffffu), (int) ((pay >> 26) & 3u));
      break;
    }
    case SG_EV_UNASSIGN:
      sg_unassign ((int) val);
      break;
    case SG_EV_BACKTRACK:
      sg_backtrack ((int) val);
      break;
    case SG_EV_GONE:
      sg_gone ((int) val, (int) (pay & 1u));
      break;
    case SG_EV_CONFLICT:
      sg_conflict ();
      break;
    case SG_EV_BACKJUMP:
      sg_backjump ((int) pay, (int) val);
      break;
    case SG_EV_LEARN:
      sg_learned ((int) pay, (int) val);
      break;
    case SG_EV_LIT:
      sg_learned_lit (pay & 1u ? -(int) val : (int) val);
      break;
    case SG_EV_RESTART:
      sg_restart ();
      break;
    case SG_EV_REDUCE:
      sg_reduce ();
      break;
    case SG_EV_RESULT:
      out.result = (int) val;
      break;
    case SG_EV_MODEL:
      if (out.model.size () <= pay)
        out.model.resize ((size_t) pay + 1, 0);
      out.model[pay] = val;
      break;
    default:
      break;
    }
  }
}

void trace_outcome_records (int result, const std::vector<uint32_t> &state, int nvars,
                            std::vector<uint32_t> &words) {
  words.push_back ((uint32_t) SG_EV_RESULT << 28);
  words.push_back ((uint32_t) result);
  if (result != 10)
    return;
  for (int base = 0; base < nvars; base += 32) {
    uint32_t bits = 0;
    for (int k = 0; k < 32 && base + k < nvars; k++)
      if ((state[(size_t) (base + k + 1)] & SG_VAL_MASK) == SG_VAL_TRUE)
        bits |= 1u << k;
    words.push_back ((uint32_t) SG_EV_MODEL << 28 | (uint32_t) (base / 32));
    words.push_back (bits);
  }
}
