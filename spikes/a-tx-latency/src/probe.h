// The measurement engine: turns emissions plus captured audio into latencies.
//
// The method the brief asks for, made concrete. One process holds both ends.
// The emission side records, on the single clock, the instant each burst was
// handed to Zoom. The capture side records the far Zoom client's own output,
// tapped locally, and maps it onto that same clock via CaptureTimebase. For
// each burst we then correlate the known waveform against the slice of
// captured audio that could plausibly contain it, and the difference between
// the correlation peak's host time and the emission's host time is the
// one-way latency. No second wall clock is consulted and no two timestamps
// from different sources are ever subtracted.
//
// What this number includes, stated plainly because it is the difference
// between a defensible result and a confident guess: it runs from our send()
// call to the point the far client's audio reaches the OS mixer. That covers
// Zoom's encode, network, cloud, decode and jitter buffer -- all of Zoom -- and
// also a small amount of local capture-side plumbing that is ours, not Zoom's.
// `--calibrate` measures an upper bound on that local part so the report can
// bracket the true figure rather than assert a single unqualified one.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "correlator.h"
#include "timebase.h"

namespace zc {

struct ProbeConfig {
  // Search window for a burst, relative to its emission. The upper bound is
  // well past the 250 ms kill threshold on purpose: a run that comes back at
  // 900 ms is a result, and a search window that could not have seen it would
  // have reported a failure instead.
  double min_latency_ms = 0.0;
  double max_latency_ms = 1200.0;
  // Extra grace before giving up on a burst, to allow for capture buffering.
  double resolve_grace_ms = 500.0;
  DetectorConfig detector;
};

struct LatencySample {
  int32_t burst_id = 0;
  double latency_ms = 0.0;
  double peak = 0.0;
  double psr = 0.0;
};

struct ProbeStats {
  uint64_t emitted = 0;
  uint64_t resolved = 0;
  uint64_t no_detection = 0;  // window searched, nothing cleared the gate
  uint64_t data_gap = 0;      // capture data aged out before it could be searched
  uint64_t pending = 0;
  double capture_rms = 0.0;  // distinguishes "no audio" from "audio, no match"
  double measured_capture_rate_hz = 0.0;
  // The best correlation any failed burst achieved, and the gates it faced.
  // This is what separates "the probe is not in this audio at all" (peak near
  // the noise floor) from "the probe is there but degraded below the gate"
  // (peak just under min_peak, or PSR just under min_psr) -- two failures
  // with opposite fixes.
  double best_failed_peak = 0.0;
  double best_failed_psr = 0.0;
};

class LatencyProbe {
 public:
  LatencyProbe(const std::vector<float>* burst_up,
               const std::vector<float>* burst_down, const ProbeConfig& cfg);

  // From the TX pacer, at the moment of handover.
  void OnEmission(int32_t burst_id, bool up, int64_t host_ns);

  // From the capture callback. `host_ns` must be read at the top of the
  // callback, before any work, so lock contention here cannot bias it.
  void OnCapture(const float* mono, int count, int64_t host_ns);

  // Resolves whatever is now resolvable. Safe to call from any thread; does
  // the correlation work, so it is called from a worker rather than from
  // either audio path.
  void Resolve();

  std::vector<LatencySample> samples() const;
  ProbeStats stats() const;

 private:
  struct Pending {
    int32_t burst_id;
    bool up;
    int64_t emit_ns;
  };

  const std::vector<float>* up_;
  const std::vector<float>* down_;
  ProbeConfig cfg_;

  mutable std::mutex m_;
  CaptureTimebase timebase_;
  std::vector<float> capture_;      // rolling window of captured mono audio
  uint64_t capture_base_index_ = 0; // cumulative index of capture_[0]
  uint64_t capture_total_ = 0;      // cumulative frames ever captured
  std::vector<Pending> pending_;
  std::vector<LatencySample> samples_;
  ProbeStats stats_;
};

}  // namespace zc
