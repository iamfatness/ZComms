#include "frame_accumulator.h"

#include <algorithm>
#include <cstring>

namespace zc {

FrameAccumulator::FrameAccumulator()
    : partial_(static_cast<size_t>(kFrameSamples), 0.0f) {}

void FrameAccumulator::Push(const float* samples, int count, const FrameFn& fn) {
  if (samples == nullptr || count <= 0) return;
  samples_in_ += static_cast<uint64_t>(count);

  int offset = 0;
  while (offset < count) {
    const size_t want = static_cast<size_t>(kFrameSamples) - fill_;
    const size_t have = static_cast<size_t>(count - offset);

    if (have < want) {
      // Not enough to complete a frame; carry the remainder.
      std::memcpy(partial_.data() + fill_, samples + offset, have * sizeof(float));
      fill_ += have;
      return;
    }

    std::memcpy(partial_.data() + fill_, samples + offset, want * sizeof(float));
    offset += static_cast<int>(want);
    fill_ = 0;

    if (fn) fn(partial_.data(), next_frame_start_);
    next_frame_start_ += static_cast<uint64_t>(kFrameSamples);
    ++frames_out_;
  }
}

}  // namespace zc
