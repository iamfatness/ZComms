// Acoustic echo cancellation, wrapping speexdsp's MDF canceller.
//
// Why this exists (plan §2, the ship-blocker): audio ZComms sends into Zoom
// is raw PCM, so Zoom's own echo canceller never touches it. An operator on
// open speakers plays their monitor -- sidetone today, meeting audio when RX
// lands -- into their own microphone, and without this stage that loop goes
// straight into the talkback channel for every panelist to hear.
//
// Why the reference is exact rather than guessed: ZComms renders the
// operator's monitor itself, so the far-end reference is literally the
// buffer handed to the output device. Cancellation conditions do not get
// better than that -- known reference, short and stable acoustic delay, one
// machine's clocks.
//
// Threading: the mic and monitor callbacks arrive on two different device
// threads, and the speex state is not thread-safe, so both entry points take
// one mutex. Hold times are tens of microseconds at 10 ms cadences; the
// audio path never waits on anything slower than the other callback.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace zc {

class EchoCanceller {
 public:
  // `frame_samples` is the processing block (the engine's 20 ms / 960).
  // `tail_ms` is how much acoustic delay the filter can absorb; 200 ms
  // covers speakers across a desk with margin.
  EchoCanceller(int sample_rate, int frame_samples, int tail_ms);
  ~EchoCanceller();

  // The far end: audio just written to the output device. Arbitrary length;
  // buffered internally to the frame size.
  void FeedPlayback(const float* samples, int count);

  // The near end: one full frame of mic input, cancelled in place.
  void ProcessCapture(float* frame);

  // Bypass switch. When disabled the capture path is untouched and playback
  // feed is discarded, but the object stays alive so re-enabling is instant.
  void SetEnabled(bool on);
  bool enabled() const { return enabled_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::mutex m_;
  bool enabled_ = true;
  int frame_samples_;
};

}  // namespace zc
