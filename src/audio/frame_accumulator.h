// Chunks arbitrary capture-callback sizes into exact 20 ms frames.
//
// A capture device hands over whatever block size it feels like -- 441, 480,
// 1024, and not necessarily the same size twice. The TX path needs exactly
// kFrameSamples per frame, forever, because that cadence is what Zoom is being
// fed on. Something has to sit between them and it should be one obvious
// place rather than ad-hoc leftover handling in each callback.
//
// Plan §5: the frame index here is derived from samples counted, never from
// when a callback arrived. Callback arrival jitters; sample counts do not. Any
// timestamp downstream is a function of this counter, so the audio clock stays
// tied to the audio rather than to the scheduler.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "audio_defs.h"

namespace zc {

class FrameAccumulator {
 public:
  // `start_sample` is the cumulative index of the frame's first sample.
  using FrameFn = std::function<void(const float* frame, uint64_t start_sample)>;

  FrameAccumulator();

  // Emits zero or more complete frames, calling `fn` for each. Any remainder
  // is carried to the next call.
  void Push(const float* samples, int count, const FrameFn& fn);

  uint64_t samples_in() const { return samples_in_; }
  uint64_t frames_out() const { return frames_out_; }
  // Samples held back waiting to complete a frame. Bounded by construction:
  // always < kFrameSamples.
  int pending() const { return static_cast<int>(fill_); }

 private:
  std::vector<float> partial_;
  size_t fill_ = 0;
  uint64_t samples_in_ = 0;
  uint64_t frames_out_ = 0;
  uint64_t next_frame_start_ = 0;
};

}  // namespace zc
