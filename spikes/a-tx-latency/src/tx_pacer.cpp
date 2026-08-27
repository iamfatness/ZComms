#include "tx_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "clock.h"

namespace zc {
namespace {

// Windows' default timer resolution is ~15.6 ms, which a 20 ms cadence cannot
// hit even approximately. main() raises it to 1 ms, and the last stretch is
// spun rather than slept so the tick lands close to the grid regardless of what
// the scheduler grants. The spin window is deliberately short: this costs a
// fraction of one core, and buying tick accuracy with it is worth more than the
// CPU, because a virtual mic whose cadence wanders is the case Zoom handles
// worst.
constexpr int64_t kSpinWindowNs = 1'500'000;  // 1.5 ms

void SleepUntilNs(int64_t target_ns) {
  const int64_t coarse_until = target_ns - kSpinWindowNs;
  int64_t now = NowNs();
  if (now < coarse_until) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(coarse_until - now));
  }
  while ((now = NowNs()) < target_ns) {
    std::this_thread::yield();
  }
}

}  // namespace

TxPacer::TxPacer(FrameRing* ring, FrameSink* sink, EmissionFn on_emit)
    : ring_(ring), sink_(sink), on_emit_(std::move(on_emit)) {}

TxPacer::~TxPacer() { Stop(); }

bool TxPacer::Start(int prime_frames, int prime_timeout_ms) {
  bool primed = true;
  const int64_t deadline = NowNs() + static_cast<int64_t>(prime_timeout_ms) * 1'000'000;
  while (static_cast<int>(ring_->size()) < prime_frames) {
    if (NowNs() > deadline) {
      primed = false;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(&TxPacer::Run, this);
  return primed;
}

void TxPacer::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (thread_.joinable()) thread_.join();
}

void TxPacer::Run() {
  static const int16_t kSilence[kFrameSamples] = {};

  const int64_t start_ns = NowNs();
  const int64_t period_ns = static_cast<int64_t>(kFrameMs) * 1'000'000;
  uint64_t tick = 0;

  while (running_.load(std::memory_order_acquire)) {
    // Absolute grid. Never "now + period" -- see the header comment.
    const int64_t target_ns = start_ns + static_cast<int64_t>(++tick) * period_ns;
    SleepUntilNs(target_ns);

    const int64_t actual_ns = NowNs();
    const double late_ms = NsToMs(actual_ns - target_ns);

    TxFrame frame;
    const bool have_frame = ring_->Pop(&frame);
    const bool can_send = sink_->CanSend();

    {
      std::lock_guard<std::mutex> lock(stats_m_);
      ++stats_.ticks;
      late_sum_ms_ += late_ms;
      stats_.max_late_ms = std::max(stats_.max_late_ms, late_ms);
      stats_.mean_late_ms = late_sum_ms_ / static_cast<double>(stats_.ticks);
      if (!have_frame) ++stats_.underruns;
      if (!can_send) ++stats_.gated_ticks;
    }

    if (!can_send) {
      // The window is shut. The tick still happened and is counted; we simply
      // have nothing legal to do with the frame. Dropping it rather than
      // holding it keeps the stream aligned to realtime when the window
      // reopens instead of replaying stale audio.
      continue;
    }

    const int16_t* pcm = have_frame ? frame.pcm : kSilence;

    // Timestamp taken immediately before the handover. The quantity being
    // measured is when audio entered Zoom, so it is anchored to the call that
    // does the entering -- not to the tick we aimed at, and not to when the
    // producer generated the frame.
    const int64_t emit_ns = NowNs();
    const bool ok = sink_->Send(pcm, kFrameSamples);

    {
      std::lock_guard<std::mutex> lock(stats_m_);
      if (ok) {
        ++stats_.sends;
      } else {
        ++stats_.send_errors;
      }
    }

    if (ok && have_frame && frame.burst_id >= 0 && on_emit_) {
      // The onset sits burst_offset samples into the frame, so the burst's own
      // emission time is the frame's plus that offset. Frame-aligned bursts
      // make this zero today; carrying it anyway keeps the alignment an
      // implementation choice rather than a hidden assumption.
      const int64_t onset_ns =
          emit_ns + static_cast<int64_t>(
                        (static_cast<double>(frame.burst_offset) / kSampleRate) * 1e9);
      on_emit_(frame.burst_id, frame.burst_up, onset_ns);
    }
  }
}

PacerStats TxPacer::stats() const {
  std::lock_guard<std::mutex> lock(stats_m_);
  return stats_;
}

}  // namespace zc
