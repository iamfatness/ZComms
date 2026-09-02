// LogParts -- the log's size policy: when a part is full, which part comes
// next, and what each one is called.
//
// One session left a 43.9 MB file under %APPDATA%\ZComms\logs. Most of that
// was the status meter (console-only now, see diag_line.h), but a long show
// with a chatty SDK can still fill a file, and "the log ate the disk" is not a
// failure mode a broadcast tool gets to have. So a run writes at most
// kMaxParts parts of kMaxPartBytes each, CYCLING: part 3 rolls back over part
// 0, which is reopened CREATE_ALWAYS and therefore truncated. That wrap is the
// only thing bounding the run, and it is the part nobody had watched happen --
// the first roll had been verified, the cycle had not.
//
// Pure so the cycle can be pinned by a test rather than by a four-hour show.
// (The truncate-on-reuse half is CreateFileA's, and is proven by driving the
// app with a tiny cap; see the PR that added this file.)
#pragma once

#include <string>

namespace zc {

class LogParts {
 public:
  LogParts(long long max_part_bytes, int max_parts)
      : max_bytes_(max_part_bytes), max_parts_(max_parts) {}

  int part() const { return part_; }
  long long bytes() const { return bytes_; }

  // The part a roll lands on. WRAPPED: with 4 parts, part 3's successor is
  // part 0, not part 4 -- the note the log prints when a part fills says where
  // to look next, and it said "part 4" for a file that does not exist.
  int next_part() const { return (part_ + 1) % max_parts_; }

  // Account for a write. True when the part is now full and the caller must
  // roll (announce next_part(), then Advance() and reopen).
  bool Wrote(long long n) {
    bytes_ += n;
    return bytes_ >= max_bytes_;
  }

  void Advance() {
    part_ = next_part();
    bytes_ = 0;
  }

  // Part 0 keeps the plain name so the common case -- a run short enough to
  // never roll -- has no numeric suffix to explain to whoever is reading it.
  static std::string Name(const std::string& stamp, int part) {
    if (part == 0) return "\\zcomms-" + stamp + ".log";
    return "\\zcomms-" + stamp + "." + std::to_string(part) + ".log";
  }

 private:
  long long max_bytes_;
  int max_parts_;
  int part_ = 0;
  long long bytes_ = 0;
};

}  // namespace zc
