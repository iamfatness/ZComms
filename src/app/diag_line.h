// DiagSplitter -- the rule that decides which bytes of the diagnostic stream
// reach the log file and which are console-only decoration.
//
// Why this exists: one session left a 43.9 MB log under %APPDATA%\ZComms\logs.
// Almost all of it was the main loop's status meter, a single
// printf("\r  [====] TALK  keys 01 ...") repainted every tick -- ~33 times a
// second, ~2 KB/s, and in a FILE it is not 33 lines, it is one unreadable
// line 43 MB long, because \r overwrites nothing on disk. A show that runs
// four hours produced a log nobody can open, hiding the handful of lines that
// are actually evidence.
//
// The rule, and it is the whole module:
//
//   * a segment ending in '\n' is a real log line   -> file AND console
//   * a segment ending in '\r' is a status repaint  -> console ONLY
//
// That keeps the live meter for whoever is watching a terminal, keeps every
// genuine line in the file, and costs the file nothing per tick. Pure and
// header-only so the tests can pin it without linking any Win32.
#pragma once

#include <string>
#include <vector>

namespace zc {

struct DiagChunk {
  std::string text;
  bool to_file = false;
  bool to_console = false;
};

class DiagSplitter {
 public:
  // Feed raw bytes as they come off the stream. Returns the chunks that are
  // ready to emit, each already routed. Bytes with no terminator yet are held
  // until one arrives -- except that a partial line never grows without bound
  // (a module printing megabytes with no newline would otherwise buffer them
  // all), so it is flushed to both sinks at kMaxHeld.
  std::vector<DiagChunk> Feed(const char* data, size_t len) {
    std::vector<DiagChunk> out;
    for (size_t i = 0; i < len; ++i) {
      const char c = data[i];
      held_ += c;
      if (c == '\n') {
        out.push_back({held_, true, true});
        held_.clear();
      } else if (c == '\r') {
        // A status repaint. Console-only: on disk it would append forever.
        out.push_back({held_, false, true});
        held_.clear();
      } else if (held_.size() >= kMaxHeld) {
        out.push_back({held_, true, true});
        held_.clear();
      }
    }
    return out;
  }

  // Everything still held, emitted as a real line. Used when the stream ends
  // or before a deliberate termination -- a FATAL line that never got its
  // newline is still the most important line in the file.
  DiagChunk Drain() {
    DiagChunk c{held_, !held_.empty(), !held_.empty()};
    held_.clear();
    return c;
  }

  bool empty() const { return held_.empty(); }

  static constexpr size_t kMaxHeld = 8192;

 private:
  std::string held_;
};

}  // namespace zc
