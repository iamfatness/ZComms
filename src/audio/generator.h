// Produces the TX signal into the frame ring on its own schedule.
//
// Kept separate from the pacer on purpose. If the TX thread synthesised its
// own audio it could never starve, and the underrun path -- one of the three
// behaviours plan §6.1 wants proven -- would never execute. This also mirrors
// the eventual shape, where the producer is a capture callback whose timing
// the TX thread does not control.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "frame_ring.h"
#include "signal.h"

namespace zc {

class SignalGenerator {
 public:
  SignalGenerator(FrameRing* ring, const SignalParams& params);
  ~SignalGenerator();

  void Start();
  void Stop();

  // The reference waveforms the detector correlates against. Handing out the
  // exact emitted samples -- rather than regenerating them in the detector --
  // means the two can never disagree about what was sent.
  const std::vector<float>& burst_up() const { return burst_up_; }
  const std::vector<float>& burst_down() const { return burst_down_; }

  uint64_t bursts_queued() const { return bursts_queued_.load(); }

 private:
  void Run();

  FrameRing* ring_;
  SignalParams params_;
  std::vector<float> burst_up_;
  std::vector<float> burst_down_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> bursts_queued_{0};
};

}  // namespace zc
