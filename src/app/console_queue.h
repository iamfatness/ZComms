// ConsoleQueue -- the bounded, DROPPING hand-off between whoever called printf
// and the thread that writes to the console.
//
// This is the one behaviour standing between the app and a hang we have
// actually seen. v0.1.10 wedged on the owner's machine (WER: AppHangB1) and a
// live reproduction put thread 0 -- the MAIN loop -- here:
//
//   ntdll!NtWriteFile / KERNELBASE!WriteFile / ucrtbase!write_nolock
//   ucrtbase!common_vfprintf / ucrtbase!_stdio_common_vfprintf / zcomms
//
// The app's own status output had filled a 64 KB inherited pipe that nobody
// was draining, and printf blocked FOREVER holding the CRT's stdout lock. A
// console wedges the same way with nothing broken at all: select text in a
// QuickEdit console and every writer to it stops dead.
//
// So no thread of this app writes to a console. It hands the line here and
// returns, ALWAYS: at capacity Push drops and counts, it never waits for
// space, never grows past the cap. A dropped console line costs nothing --
// the log FILE has every line, and it is the file that is evidence -- while a
// blocked main loop is the show.
//
// The drop is at the TAIL (the arriving line), not the head. When the sink is
// wedged nothing already queued is moving either, so there is no version of
// this that keeps the console current; what matters is that the push is O(1)
// with no reshuffle and that the loss is counted rather than silent.
//
// The lock and the condition variable live IN here, unlike this codebase's
// other extracted units, which are pure policy the caller drives. They have to:
// "a push returns even while the consumer never will" is a property OF the
// synchronisation, and if the waiting lived in the caller then the test would
// be pinning a copy of the app's hand-off rather than the hand-off. There is
// exactly one place a future refactor could wait for space, and a test drives
// it directly.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace zc {

class ConsoleQueue {
 public:
  explicit ConsoleQueue(size_t capacity) : capacity_(capacity) {}

  // Returns false when the line was dropped. NEVER waits for space: that is
  // the whole module, and the reason the main loop cannot be taken down by a
  // console nobody is reading.
  bool Push(std::string text) {
    {
      std::lock_guard<std::mutex> lock(m_);
      if (q_.size() >= capacity_) {
        ++dropped_;
        return false;
      }
      q_.push_back(std::move(text));
    }
    cv_.notify_one();
    return true;
  }

  // Blocks until there is a line to write or the queue is closed; false means
  // closed and drained, i.e. the consumer should stop. Waiting HERE is fine --
  // this is the console thread, and being stuck on the console is its job.
  bool PopWait(std::string* out) {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;
    if (out != nullptr) *out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  // Wakes every waiter and makes PopWait return false once drained.
  void Close() {
    {
      std::lock_guard<std::mutex> lock(m_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_);
    return q_.size();
  }

  size_t capacity() const { return capacity_; }

  // Counted, not silently lost: the pump writes this into the log file so a
  // reader knows the console tail is incomplete and the file is not.
  unsigned long long dropped() const {
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  size_t capacity_;
  std::deque<std::string> q_;
  unsigned long long dropped_ = 0;
  bool closed_ = false;
};

}  // namespace zc
