#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "console_queue.h"
#include "diag_line.h"
#include "log_parts.h"
#include "stall_watch.h"
#include "test_util.h"

// The pieces of the hang-diagnosability work that are pure policy, and
// therefore the ones that can be pinned without reproducing a hang. Everything
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

static void TestConsoleQueue() {
  ZC_TEST("console: pushing past capacity returns, and counts the loss");
  {
    zc::ConsoleQueue q(4);
    for (int i = 0; i < 4; ++i) ZC_CHECK(q.Push("line\n"));
    ZC_CHECK(q.size() == 4);
    ZC_CHECK(!q.Push("one too many\n"));   // dropped, not queued, not blocked
    ZC_CHECK(!q.Push("and another\n"));
    ZC_CHECK(q.size() == q.capacity());    // never exceeds the cap
    ZC_CHECK(q.dropped() == 2u);            // counted, not silently lost
  }

  ZC_TEST("console: the drop is at the TAIL -- the backlog is kept in order");
  // Which end matters for reading a log next to a console: the queue holds the
  // OLDEST lines it accepted, in order, and the arriving line is what goes.
  // (The alternative, drop-oldest, would reshuffle on every drop while the
  // sink is wedged and buys nothing -- the FILE is the record either way.)
  {
    zc::ConsoleQueue q(2);
    q.Push("first\n");
    q.Push("second\n");
    q.Push("third\n");  // dropped
    q.Close();          // so the drained PopWait returns instead of waiting
    std::string s;
    ZC_CHECK(q.PopWait(&s) && s == "first\n");
    ZC_CHECK(q.PopWait(&s) && s == "second\n");
    ZC_CHECK(!q.PopWait(&s));
    ZC_CHECK(q.dropped() == 1u);
  }

  ZC_TEST("console: a drained queue accepts again");
  // A console that un-wedges must start carrying lines again on its own; the
  // drop is a momentary policy, not a latched dead state.
  {
    zc::ConsoleQueue q(2);
    q.Push("a\n");
    q.Push("b\n");
    ZC_CHECK(!q.Push("c\n"));
    std::string s;
    q.Close();
    q.PopWait(&s);
    ZC_CHECK(q.Push("d\n"));
    ZC_CHECK(q.size() == 2);
    ZC_CHECK(q.dropped() == 1u);  // the count is a run total, not a level
  }

  ZC_TEST("console: a WEDGED console cannot block the thread that printed");
  // THE test. v0.1.10 hung with thread 0 -- the main loop -- in NtWriteFile
  // under _stdio_common_vfprintf: its own output had filled a 64 KB pipe
  // nobody drained and printf never returned, holding the CRT stdout lock.
  // This reproduces that shape exactly -- the consumer takes one line and
  // never comes back, which is what selecting text in a QuickEdit console
  // does -- and asserts the producer still finishes. An enqueue that waited
  // for space would never return here at all.
  {
    constexpr int kPushes = 100000;
    struct Wedge {
      zc::ConsoleQueue q{256};
      std::atomic<bool> wedged{false};
      std::atomic<bool> release{false};
      std::promise<int> accepted;
    };
    // On the heap, and deliberately leaked if the producer never comes back: a
    // thread parked inside a queue whose storage went out of scope crashes the
    // run on the way out, and by then the failure is already reported.
    Wedge* w = new Wedge();
    std::future<int> accepted = w->accepted.get_future();

    // Same shape as diag_log.cpp's ConsolePumpThread -- PopWait a line, write
    // it -- except this one never comes back from the write.
    std::thread sink([w] {
      std::string s;
      w->q.PopWait(&s);
      w->wedged.store(true);
      while (!w->release.load()) std::this_thread::yield();
    });
    w->q.Push("wedge me\n");
    while (!w->wedged.load()) std::this_thread::yield();

    const auto t0 = std::chrono::steady_clock::now();
    std::thread producer([w] {
      int n = 0;
      for (int i = 0; i < kPushes; ++i) {
        if (w->q.Push("[zcomms] a line the console will never show\n")) ++n;
      }
      w->accepted.set_value(n);
    });
    // Bounded, so a reintroduced blocking enqueue FAILS the suite instead of
    // hanging it -- a hung CI job is a worse instrument than a red one.
    const bool returned = accepted.wait_for(std::chrono::seconds(5)) ==
                          std::future_status::ready;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    w->release.store(true);
    sink.join();
    ZC_CHECK(returned);  // false = the enqueue waited for space
    if (!returned) {
      producer.detach();
      return;
    }
    producer.join();
    const int n = accepted.get();

    ZC_CHECK(ms < 2000);                       // returned, and promptly
    ZC_CHECK(w->q.size() == w->q.capacity());  // bounded under the flood
    ZC_CHECK(n == 256);                        // the wedged slot never freed
    ZC_CHECK(w->q.dropped() ==                 // every loss accounted for
             static_cast<unsigned long long>(kPushes - n));
    delete w;
  }
}

static void TestLogParts() {
  ZC_TEST("log: part 0 keeps the plain name, later parts are numbered");
  {
    ZC_CHECK(zc::LogParts::Name("20260902-141500", 0) ==
             "\\zcomms-20260902-141500.log");
    ZC_CHECK(zc::LogParts::Name("20260902-141500", 3) ==
             "\\zcomms-20260902-141500.3.log");
  }

  ZC_TEST("log: a part rolls only at the cap");
  {
    zc::LogParts p(100, 4);
    ZC_CHECK(!p.Wrote(60));
    ZC_CHECK(p.bytes() == 60);
    ZC_CHECK(p.part() == 0);
    ZC_CHECK(p.Wrote(40));  // exactly at the cap is full
    p.Advance();
    ZC_CHECK(p.part() == 1);
    ZC_CHECK(p.bytes() == 0);  // a reused part starts empty
  }

  ZC_TEST("log: the last part WRAPS back over part 0");
  // The bound on a run's log is this wrap, and it had never been watched
  // happen -- only the first roll had. It also caught a real defect: the
  // "continuing in part N" note said part 4, a file that never exists.
  {
    zc::LogParts p(10, 4);
    for (int i = 1; i < 4; ++i) {
      ZC_CHECK(p.next_part() == i);
      p.Wrote(10);
      p.Advance();
      ZC_CHECK(p.part() == i);
    }
    ZC_CHECK(p.next_part() == 0);  // NOT 4
    p.Wrote(10);
    p.Advance();
    ZC_CHECK(p.part() == 0);
    ZC_CHECK(p.bytes() == 0);
  }

  ZC_TEST("log: the cycle keeps cycling, forever, at a fixed number of parts");
  {
    zc::LogParts p(10, 4);
    for (int i = 0; i < 42; ++i) {
      p.Wrote(10);
      p.Advance();
      ZC_CHECK(p.part() >= 0 && p.part() < 4);
    }
    ZC_CHECK(p.part() == 2);  // 42 rolls from part 0
  }
}

void TestDiag() {
  std::printf("TestDiag\n");
  TestDiagSplitter();
  TestStallWatch();
  TestConsoleQueue();
  TestLogParts();
}
