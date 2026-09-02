// StallWatch -- "this loop stopped beating" as a pure, testable policy.
//
// Two loops in this app must never stop: the main loop (Zoom SDK pump, keying,
// state publish) and the shell window's message pump. Both have now been seen
// to stop. The 2026-08-28 event stalled the MAIN loop; v0.1.10 stalled
// something Windows blamed on the UI thread and logged as AppHangB1 ("zcomms
// .exe stopped interacting with Windows and was closed") -- a WER class that
// leaves NO minidump and no crash-trap line, because nothing faulted.
//
// A watchdog cannot fix either. What it can do is make the next occurrence
// name itself: how long, which thread, and what the other loop was doing at
// the time. The policy is separated from the Win32 plumbing so the parts with
// edge cases (report once at the crossing, keep re-reporting while it lasts,
// say how long it lasted when it ends) get pinned by unit tests instead of by
// staring at a hang that reproduces once a week on somebody else's machine.
#pragma once

#include <cstdint>

namespace zc {

class StallWatch {
 public:
  // threshold_ms: silence longer than this is a stall. Pick it well above the
  // loop's worst legitimate iteration -- the main loop's Pump(30) plus a
  // device enumeration plus an engine restart is comfortably under 5 s.
  explicit StallWatch(int64_t threshold_ms) : threshold_ms_(threshold_ms) {}

  // Called by the watched loop, every iteration.
  void Beat(int64_t now_ms) {
    last_beat_ms_ = now_ms;
    started_ = true;
  }

  // Called by the watchdog thread on its own cadence. Returns true when the
  // caller should emit a stall line, and always fills *stalled_ms with how
  // long the loop has been silent.
  //
  // Reports at the crossing and then once per threshold for as long as the
  // stall lasts: a hang that runs until Windows kills the process must leave a
  // TRAIL in the log, not a single line a reader could mistake for a blip that
  // recovered. The v0.1.10 run left neither -- there was no log at all.
  bool Poll(int64_t now_ms, int64_t* stalled_ms) {
    const int64_t age = started_ ? now_ms - last_beat_ms_ : 0;
    if (stalled_ms != nullptr) *stalled_ms = age;
    if (!started_ || age < threshold_ms_) {
      stalled_ = false;
      next_report_ms_ = 0;
      return false;
    }
    stalled_ = true;
    if (age > worst_ms_) worst_ms_ = age;
    if (now_ms < next_report_ms_) return false;
    next_report_ms_ = now_ms + threshold_ms_;
    return true;
  }

  // True exactly once after a stall ends, filling *lasted_ms with the worst
  // age seen during it. The recovery line is what separates "it froze for
  // nine seconds and came back" from "it never came back" -- and only the
  // first of those is survivable, so the log has to distinguish them.
  bool ConsumeRecovery(int64_t* lasted_ms) {
    if (stalled_ || worst_ms_ == 0) return false;
    if (lasted_ms != nullptr) *lasted_ms = worst_ms_;
    worst_ms_ = 0;
    return true;
  }

  bool stalled() const { return stalled_; }

 private:
  int64_t threshold_ms_;
  int64_t last_beat_ms_ = 0;
  int64_t next_report_ms_ = 0;
  int64_t worst_ms_ = 0;
  bool started_ = false;
  bool stalled_ = false;
};

}  // namespace zc
