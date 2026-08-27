// The fixed-cadence TX thread from plan §6.1.
//
// Non-negotiable properties, all three of which are behaviours rather than
// implementation details, and all three of which the real virtual-mic
// component inherits from here:
//
//   The clock is fixed at 20 ms and absolute. Ticks are computed as
//   start + n*20ms, never as "now + 20ms" -- the latter accumulates every
//   scheduling overshoot into permanent drift, and a TX stream that slowly
//   walks away from realtime is exactly the failure this harness would
//   misreport as rising latency.
//
//   Underrun is a counted condition, never a stall. No frame at the tick means
//   send silence and increment. A virtual mic that simply stops calling send()
//   is the behaviour Zoom handles worst, and a silent stall in the harness
//   would look like a lost burst rather than like starvation.
//
//   Every send() is gated on the onMicStartSend/onMicStopSend window. Outside
//   it the tick still happens and is counted -- the clock does not pause
//   because Zoom is not listening, or the first tick after the window opens
//   would arrive at an arbitrary phase.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "audio_defs.h"
#include "frame_ring.h"

namespace zc {

// The destination for paced frames. Zoom's virtual mic is one implementation;
// the calibration path and the self-test use others, which is what lets the
// pacing be exercised without a meeting.
class FrameSink {
 public:
  virtual ~FrameSink() = default;
  // The send window. False means a tick is counted but no audio is handed over.
  virtual bool CanSend() = 0;
  virtual bool Send(const int16_t* pcm, int samples) = 0;
};

struct PacerStats {
  uint64_t ticks = 0;
  uint64_t sends = 0;
  uint64_t underruns = 0;    // tick with no frame ready -> silence sent
  uint64_t gated_ticks = 0;  // tick outside the send window
  uint64_t send_errors = 0;
  double max_late_ms = 0.0;  // worst tick lateness against the absolute grid
  double mean_late_ms = 0.0;
};

class TxPacer {
 public:
  // Called when a marked frame is handed to the sink, with the host time of
  // the marked instant. Optional -- live audio is unmarked and never calls it.
  using EmissionFn =
      std::function<void(int32_t mark_id, bool mark_flag, int64_t host_ns)>;

  TxPacer(FrameRing* ring, FrameSink* sink, EmissionFn on_emit);
  ~TxPacer();

  // Waits for `prime_frames` to be queued before the first tick, so ordinary
  // producer jitter does not underrun the very first sends (plan §6.1 asks for
  // 2-3 frames of priming). Returns false if priming timed out; the pacer
  // starts anyway, because starving is a countable condition and not a reason
  // to refuse to run.
  bool Start(int prime_frames, int prime_timeout_ms);
  void Stop();

  PacerStats stats() const;

 private:
  void Run();

  FrameRing* ring_;
  FrameSink* sink_;
  EmissionFn on_emit_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex stats_m_;
  PacerStats stats_;
  double late_sum_ms_ = 0.0;
};

}  // namespace zc
