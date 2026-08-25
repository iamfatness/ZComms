#include "generator.h"

#include <algorithm>
#include <chrono>

#include "audio_defs.h"
#include "clock.h"

namespace zc {

SignalGenerator::SignalGenerator(FrameRing* ring, const SignalParams& params)
    : ring_(ring),
      params_(params),
      burst_up_(MakeBurst(params, true)),
      burst_down_(MakeBurst(params, false)) {}

SignalGenerator::~SignalGenerator() { Stop(); }

void SignalGenerator::Start() {
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(&SignalGenerator::Run, this);
}

void SignalGenerator::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (thread_.joinable()) thread_.join();
}

void SignalGenerator::Run() {
  const int frames_per_period =
      std::max(1, static_cast<int>(params_.period_ms / kFrameMs));
  NoiseBed bed(params_.bed_dbfs);

  std::vector<float> scratch(static_cast<size_t>(kFrameSamples));
  uint64_t seq = 0;
  int32_t burst_id = 0;

  // Runs a little ahead of the pacer so the ring stays fed; the ring's bound
  // plus drop-oldest is what stops "a little ahead" turning into unbounded
  // buffering if the pacer is gated for a long time.
  const int64_t period_ns = static_cast<int64_t>(kFrameMs) * 1'000'000;
  const int64_t start_ns = NowNs();
  int64_t produced = 0;

  while (running_.load(std::memory_order_acquire)) {
    const int frame_in_period = static_cast<int>(seq % frames_per_period);

    // The comfort-noise bed underlies everything, including the burst. Zoom's
    // VAD sees a continuously active stream rather than isolated events.
    bed.Fill(scratch.data(), kFrameSamples);

    TxFrame f;
    f.seq = seq;
    f.burst_id = -1;
    f.burst_offset = 0;

    // Bursts are frame-aligned and may span more than one frame; frame 0 of
    // each period carries the onset, subsequent frames carry the remainder.
    const int burst_frames =
        static_cast<int>((burst_up_.size() + kFrameSamples - 1) / kFrameSamples);
    if (frame_in_period < burst_frames) {
      const bool up = (burst_id % 2) == 0;
      const std::vector<float>& burst = up ? burst_up_ : burst_down_;
      const size_t offset = static_cast<size_t>(frame_in_period) * kFrameSamples;
      for (int i = 0; i < kFrameSamples; ++i) {
        const size_t idx = offset + static_cast<size_t>(i);
        if (idx < burst.size()) scratch[static_cast<size_t>(i)] += burst[idx];
      }
      if (frame_in_period == 0) {
        f.burst_id = burst_id;
        f.burst_up = up;
        bursts_queued_.fetch_add(1);
      }
      if (frame_in_period == burst_frames - 1) ++burst_id;
    }

    for (int i = 0; i < kFrameSamples; ++i) {
      f.pcm[i] = FloatToPcm16(scratch[static_cast<size_t>(i)]);
    }
    ring_->Push(f);
    ++seq;

    // Pace production to realtime against the absolute grid, for the same
    // drift reason as the pacer.
    ++produced;
    const int64_t target = start_ns + produced * period_ns;
    const int64_t now = NowNs();
    if (now < target) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(target - now));
    }
  }
}

}  // namespace zc
