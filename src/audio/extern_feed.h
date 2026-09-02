// Extern feed: one channel (or pair) of a multichannel capture device,
// latched into one talkback channel -- the "Zoom as last mile" piece (spec
// docs/plans/2026-09-01-extern-feeds.md). A larger intercom's mix arrives
// on a device channel (Dante RX, console bus) and rides into the channel
// continuously; the operator's PTT barges over it via ChannelMix.
//
// FeedChain is the device-free processing core, so every property here is
// testable with no hardware: channel extract/downmix (the SDK is mono-only,
// law #5 -- a stereo pair is summed at the boundary, never declared) ->
// smoothed gain -> limiter (a console bus WILL run hot) -> latch envelope
// (reuse of the PTT ramp: latch edges must not click) -> 20 ms framing ->
// own ring. No AEC (line feeds have no acoustic path) and no PTT -- LATCH
// is this source's envelope.
//
// Threading: PushInterleaved runs on the device callback thread, PullFrame
// on the TX pacer thread; the ring is the boundary. Latch is an atomic the
// capture thread applies to the envelope itself.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_defs.h"
#include "envelope.h"
#include "frame_accumulator.h"
#include "frame_ring.h"
#include "limiter.h"

namespace zc {

// One feed's operator-facing description. Channels are 1-based, the way
// every console and Dante controller labels them; ch_b = -1 means single.
struct FeedConfig {
  std::string device;  // capture device name substring
  int ch_a = 1;
  int ch_b = -1;
  double gain_db = 0.0;
  bool latch = false;
};

// "device:3" or "device:3-4" (device may itself contain ':' -- the LAST
// colon splits). False on malformed channel numbers.
bool ParseFeedSpec(const std::string& spec, FeedConfig* out);
std::string FormatFeedSpec(const FeedConfig& c);

// feeds.env line payload: "<spec>,<gain_db>,<latch 0|1>".
bool ParseFeedLine(const std::string& line, FeedConfig* out);
std::string FormatFeedLine(const FeedConfig& c);

// Picks ch_a (or averages ch_a/ch_b) out of interleaved frames. Channel
// indexes are 1-based and clamped into [1, channels].
void ExtractDownmix(const float* interleaved, int frames, int channels,
                    int ch_a, int ch_b, float* mono_out);

class FeedChain {
 public:
  explicit FeedChain(const FeedConfig& cfg);

  // Device callback thread.
  void PushInterleaved(const float* interleaved, int frames, int channels);

  // Control thread. Latch ramps via the envelope on the capture thread.
  void SetLatch(bool on) { latched_.store(on); }
  bool latch() const { return latched_.load(); }
  void SetGainDb(double db);

  // TX pacer thread: one 20 ms frame, or false when none is ready (an
  // unlatched chain drains to empty; a latched one that is empty is an
  // underrun the caller counts and covers with silence).
  bool PullFrame(int16_t* out);

  const FeedConfig& config() const { return cfg_; }
  uint64_t frames_out() const { return frames_out_.load(); }
  uint64_t drops() const { return ring_.drops(); }
  // Decaying post-envelope peak (0..32767) for the panel's feed lamp: what
  // is actually reaching the channel.
  int peak() const { return peak_.load(); }
  // Decaying PRE-envelope peak (0..32767), post-gain, for the panel's input
  // meter. peak() is zero whenever the feed is down, which is precisely when
  // the operator needs to see whether a source is arriving -- you check a
  // line before you open it, not after. Taken after the gain stage so the
  // meter and the gain keys beside it agree.
  int input_peak() const { return in_peak_.load(); }

 private:
  FeedConfig cfg_;
  SmoothedGain gain_;
  Limiter limiter_;
  Envelope latch_env_;
  FrameAccumulator accum_;
  FrameRing ring_;
  std::vector<float> mono_;
  std::atomic<bool> latched_{false};
  std::atomic<int> peak_{0};
  std::atomic<int> in_peak_{0};
  std::atomic<uint64_t> frames_out_{0};
};

}  // namespace zc
