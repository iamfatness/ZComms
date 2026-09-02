#include "diag_line.h"
#include "stall_watch.h"
#include "test_util.h"

// The two pieces of the hang-diagnosability work that are pure policy, and
// therefore the two that can be pinned without reproducing a hang. Everything
// else in diag_log.cpp is Win32 plumbing whose behaviour is the plumbing's.

static void TestDiagSplitter() {
  ZC_TEST("diag: newline-terminated lines reach the file AND the console");
  {
    zc::DiagSplitter s;
    const std::string in = "[zcomms] station up\n";
    const auto out = s.Feed(in.data(), in.size());
    ZC_CHECK(out.size() == 1);
    ZC_CHECK(out[0].text == in);
    ZC_CHECK(out[0].to_file);
    ZC_CHECK(out[0].to_console);
    ZC_CHECK(s.empty());
  }

  ZC_TEST("diag: a \\r status repaint is console-only");
  // The 43.9 MB log: one printf("\r  [====] ...") per main-loop tick, ~33/s,
  // and on disk \r overwrites nothing -- the whole session became a single
  // unreadable line. The meter stays on the console; the file never sees it.
  {
    zc::DiagSplitter s;
    const std::string in = "\r  [====] TALK  keys 01  ";
    // No terminator yet: held.
    auto out = s.Feed(in.data(), in.size());
    ZC_CHECK(out.size() == 1);  // the leading \r closes the (empty) prior line
    ZC_CHECK(!out[0].to_file);
    ZC_CHECK(out[0].to_console);
    // The repaint itself is terminated by the NEXT \r.
    const std::string again = "\r  [===-] TALK  keys 01  ";
    out = s.Feed(again.data(), again.size());
    ZC_CHECK(out.size() == 1);
    ZC_CHECK(!out[0].to_file);
    ZC_CHECK(out[0].to_console);
    ZC_CHECK(out[0].text == "  [====] TALK  keys 01  \r");
  }

  ZC_TEST("diag: a real line interleaved with repaints still lands in the file");
  {
    zc::DiagSplitter s;
    const std::string in = "\rmeter\n[zcomms] audio flowing into CH 1\n";
    const auto out = s.Feed(in.data(), in.size());
    ZC_CHECK(out.size() == 3);
    ZC_CHECK(!out[0].to_file);                 // the empty pre-\r segment
    ZC_CHECK(out[1].text == "meter\n");
    ZC_CHECK(out[1].to_file);
    ZC_CHECK(out[2].text == "[zcomms] audio flowing into CH 1\n");
    ZC_CHECK(out[2].to_file);
  }

  ZC_TEST("diag: an unterminated tail is still emitted by Drain");
  // A FATAL line that never got its newline is the most important line in the
  // file; it must not die in the splitter's buffer at TerminateProcess.
  {
    zc::DiagSplitter s;
    const std::string in = "FATAL: abort() called";
    ZC_CHECK(s.Feed(in.data(), in.size()).empty());
    ZC_CHECK(!s.empty());
    const zc::DiagChunk tail = s.Drain();
    ZC_CHECK(tail.text == in);
    ZC_CHECK(tail.to_file);
    ZC_CHECK(s.empty());
  }

  ZC_TEST("diag: a partial line never grows without bound");
  {
    zc::DiagSplitter s;
    const std::string in(zc::DiagSplitter::kMaxHeld + 10, 'x');
    const auto out = s.Feed(in.data(), in.size());
    ZC_CHECK(out.size() == 1);
    ZC_CHECK(out[0].text.size() == zc::DiagSplitter::kMaxHeld);
    ZC_CHECK(out[0].to_file);
  }
}

static void TestStallWatch() {
  ZC_TEST("stall: a loop that has never beaten is not a stall");
  // Otherwise every watchdog fires once during startup, before the loop it
  // watches has run its first iteration.
  {
    zc::StallWatch w(5000);
    int64_t age = 0;
    ZC_CHECK(!w.Poll(100000, &age));
    ZC_CHECK(!w.stalled());
  }

  ZC_TEST("stall: silence past the threshold reports exactly once");
  {
    zc::StallWatch w(5000);
    w.Beat(1000);
    int64_t age = 0;
    ZC_CHECK(!w.Poll(4000, &age));   // 3 s of silence: healthy
    ZC_CHECK(age == 3000);
    ZC_CHECK(w.Poll(6500, &age));    // 5.5 s: crossed
    ZC_CHECK(age == 5500);
    ZC_CHECK(w.stalled());
    ZC_CHECK(!w.Poll(7000, &age));   // still hung, but not yet re-reported
  }

  ZC_TEST("stall: a hang that never ends leaves a trail, not one line");
  // The whole point of the instrument is that a log tail ending in repeated
  // stall lines is unmistakable evidence the process died hung -- a single
  // line reads like a blip that recovered.
  {
    zc::StallWatch w(5000);
    w.Beat(0);
    int64_t age = 0;
    ZC_CHECK(w.Poll(5000, &age));
    ZC_CHECK(!w.Poll(9000, &age));
    ZC_CHECK(w.Poll(10000, &age));   // one threshold later: reported again
    ZC_CHECK(age == 10000);
    ZC_CHECK(w.Poll(15000, &age));
  }

  ZC_TEST("stall: recovery reports the worst duration, exactly once");
  {
    zc::StallWatch w(5000);
    w.Beat(0);
    int64_t age = 0, lasted = 0;
    ZC_CHECK(w.Poll(6000, &age));
    ZC_CHECK(!w.ConsumeRecovery(&lasted));  // still hung
    w.Poll(9000, &age);                     // worst seen: 9000
    w.Beat(9500);
    ZC_CHECK(!w.Poll(9600, &age));          // beating again
    ZC_CHECK(w.ConsumeRecovery(&lasted));
    ZC_CHECK(lasted == 9000);
    ZC_CHECK(!w.ConsumeRecovery(&lasted));  // only once
  }

  ZC_TEST("stall: a recovered loop can stall again and report again");
  {
    zc::StallWatch w(1000);
    int64_t age = 0, lasted = 0;
    w.Beat(0);
    ZC_CHECK(w.Poll(1500, &age));
    w.Beat(2000);
    ZC_CHECK(!w.Poll(2100, &age));
    ZC_CHECK(w.ConsumeRecovery(&lasted));
    ZC_CHECK(w.Poll(3200, &age));  // second episode
  }
}

void TestDiag() {
  std::printf("TestDiag\n");
  TestDiagSplitter();
  TestStallWatch();
}
