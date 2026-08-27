#include "frame_ring.h"

namespace zc {

FrameRing::FrameRing(size_t capacity_frames)
    : buf_(capacity_frames ? capacity_frames : 1) {}

void FrameRing::Push(const TxFrame& f) {
  std::lock_guard<std::mutex> lock(m_);
  if (count_ == buf_.size()) {
    head_ = (head_ + 1) % buf_.size();  // drop oldest
    --count_;
    ++drops_;
  }
  const size_t tail = (head_ + count_) % buf_.size();
  buf_[tail] = f;
  ++count_;
}

bool FrameRing::Pop(TxFrame* out) {
  std::lock_guard<std::mutex> lock(m_);
  if (count_ == 0) return false;
  *out = buf_[head_];
  head_ = (head_ + 1) % buf_.size();
  --count_;
  return true;
}

size_t FrameRing::size() const {
  std::lock_guard<std::mutex> lock(m_);
  return count_;
}

uint64_t FrameRing::drops() const {
  std::lock_guard<std::mutex> lock(m_);
  return drops_;
}

}  // namespace zc
