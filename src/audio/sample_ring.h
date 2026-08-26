// A bounded sample ring for paths that cross two device clocks.
//
// Capture and playback are separate devices with separate crystals, so the
// sidetone path has a producer and a consumer that will never agree on rate
// forever. Something has to give, and the honest options are to drop or to
// stretch. This drops, and counts it.
//
// Dropping is right for a monitor path specifically: sidetone exists so an
// operator can hear themselves, and a rare discarded millisecond is
// imperceptible, whereas unbounded buffering would turn sidetone into a
// growing delay -- and hearing your own voice late is the one artefact that
// makes talking genuinely difficult.
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace zc {

class SampleRing {
 public:
  explicit SampleRing(size_t capacity)
      : buf_(capacity ? capacity : 1, 0.0f) {}

  void Write(const float* src, size_t n) {
    std::lock_guard<std::mutex> lock(m_);
    for (size_t i = 0; i < n; ++i) {
      if (count_ == buf_.size()) {
        head_ = (head_ + 1) % buf_.size();
        --count_;
        ++dropped_;
      }
      buf_[(head_ + count_) % buf_.size()] = src[i];
      ++count_;
    }
  }

  // Fills `n` samples, zero-padding whatever is not available. Returns the
  // number of real samples supplied so a caller can distinguish "quiet" from
  // "starved".
  size_t Read(float* dst, size_t n) {
    std::lock_guard<std::mutex> lock(m_);
    const size_t take = std::min(n, count_);
    for (size_t i = 0; i < take; ++i) {
      dst[i] = buf_[(head_ + i) % buf_.size()];
    }
    for (size_t i = take; i < n; ++i) dst[i] = 0.0f;
    head_ = (head_ + take) % buf_.size();
    count_ -= take;
    if (take < n) starved_ += (n - take);
    return take;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_);
    return count_;
  }
  uint64_t dropped() const {
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
  }
  uint64_t starved() const {
    std::lock_guard<std::mutex> lock(m_);
    return starved_;
  }

 private:
  mutable std::mutex m_;
  std::vector<float> buf_;
  size_t head_ = 0;
  size_t count_ = 0;
  uint64_t dropped_ = 0;
  uint64_t starved_ = 0;
};

}  // namespace zc
