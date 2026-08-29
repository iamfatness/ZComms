// The audio engine: capture -> process -> paced TX, plus a monitor mix.
//
// Plan §6.1-6.3. The chain, in order, and why the order is what it is:
//
//   capture (48k mono float, whatever block size the device likes)
//     -> input gain      smoothed, because operators move faders while live
//     -> limiter         look-ahead, because clipping into Zoom's encoder is
//                        not recoverable downstream
//     -> PTT envelope    ramped, never gated (plan §5)
//     -> sidetone tap    post-envelope, so the operator monitors what is
//                        actually being sent rather than what they hoped was
//     -> 20 ms frames    exact, regardless of device block size
//     -> ring            bounded, drop-oldest, counted (§6.2)
//     -> TX pacer        fixed 20 ms clock, underrun counted (§6.1)
//     -> FrameSink       Zoom's virtual mic, or a WAV file for testing
//
// The limiter sits before the envelope rather than after so that limiting
// behaviour does not change depending on whether PTT happens to be held --
// otherwise the gain reduction would pump against the ramp.
//
// The engine owns no UI and no meeting. It is driven entirely by the setters
// below, which is what lets a CLI, a control surface (§6.4) or an eventual
// front end all drive the same thing.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "aec.h"
#include "devices.h"
#include "envelope.h"
#include "frame_accumulator.h"
#include "frame_ring.h"
#include "limiter.h"
#include "sample_ring.h"
#include "tx_pacer.h"

namespace zc {

struct EngineConfig {
  std::string capture_device;   // substring match; empty = system default
  std::string monitor_device;
  bool monitor_enabled = true;

  double input_gain_db = 0.0;
  double sidetone_db = -12.0;
  bool sidetone_enabled = true;

  // 12 ms is inaudible as a fade but fast enough that a press does not feel
  // laggy. Plan §5 is the reason this is a ramp at all.
  double ptt_fade_ms = 12.0;

  // Acoustic echo cancellation (plan §2, the ship-blocker): cancel what our
  // own monitor output leaks back into the microphone. On by default when a
  // monitor is open, because the failure mode -- the operator's monitor in
  // every panelist's ear -- is discovered by the panelists, not the operator.
  bool aec = true;
  int aec_tail_ms = 200;

  double limiter_ceiling_dbfs = -1.0;
  double limiter_lookahead_ms = 2.0;
  double limiter_release_ms = 80.0;

  // Frames buffered before the pacer's first tick (§6.1 asks for 2-3), so
  // ordinary producer jitter does not underrun the opening sends.
  int prime_frames = 3;
  int ring_frames = 50;
};

struct EngineStats {
  uint64_t capture_frames = 0;
  uint64_t monitor_frames = 0;
  uint64_t frames_to_ring = 0;
  uint64_t ring_drops = 0;
  uint64_t sidetone_drops = 0;
  uint64_t sidetone_starved = 0;
  uint64_t limiter_engaged_samples = 0;
  double capture_peak = 0.0;  // decaying peak, for a level display
  PacerStats pacer;
};

class AudioEngine {
 public:
  AudioEngine(const EngineConfig& cfg, FrameSink* sink);
  ~AudioEngine();

  bool Start(std::string* error);
  void Stop();

  // PTT. Ramped in and out; safe to call from any thread and at any rate,
  // including faster than the ramp itself.
  void SetTalk(bool on);
  bool talking() const { return talk_.load(); }

  void SetInputGainDb(double db);
  void SetSidetoneDb(double db);
  void SetSidetoneEnabled(bool on);
  void SetAecEnabled(bool on);
  bool aec_enabled() const;

  const std::string& capture_device_name() const;
  const std::string& monitor_device_name() const;

  EngineStats stats() const;

 private:
  void OnCapture(const float* mono, int frames);
  void OnMonitor(float* out, int frames);

  EngineConfig cfg_;
  FrameSink* sink_;

  CaptureDevice capture_;
  MonitorDevice monitor_;
  FrameRing ring_;
  std::unique_ptr<TxPacer> pacer_;
  // Snapshotted when the pacer is torn down. Without this, reading stats
  // after Stop() reports zero ticks and zero sends for a run that plainly
  // sent audio -- which reads as a broken TX path rather than as a broken
  // accessor, and is exactly the wrong thing to be misled about here.
  PacerStats final_pacer_;

  FrameAccumulator accum_;
  std::unique_ptr<EchoCanceller> aec_;
  SmoothedGain gain_;
  Limiter limiter_;
  Envelope ptt_;
  SmoothedGain sidetone_gain_;
  SampleRing sidetone_;

  std::atomic<bool> talk_{false};
  std::atomic<bool> sidetone_on_{true};
  std::atomic<uint64_t> frames_to_ring_{0};
  std::atomic<double> capture_peak_{0.0};

  std::vector<float> scratch_;
};

}  // namespace zc
