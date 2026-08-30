#include "signal_outbox.h"

namespace zc {

void SignalOutbox::Push(const OutboundChat& msg) {
  std::lock_guard<std::mutex> lock(m_);
  q_.push_back(msg);
  while (q_.size() > kMaxQueued) {
    q_.pop_front();  // drop-oldest: a stale cue is worse than no cue
    ++dropped_;
  }
}

bool SignalOutbox::PopReady(int64_t now_ms, OutboundChat* out) {
  std::lock_guard<std::mutex> lock(m_);
  if (q_.empty()) return false;
  // INT64_MIN sentinel: the first send is never gated. Guard the subtraction
  // rather than the sentinel -- overflow on INT64_MIN is UB.
  if (last_send_ms_ != INT64_MIN && now_ms - last_send_ms_ < kMinSendGapMs) {
    return false;
  }
  *out = std::move(q_.front());
  q_.pop_front();
  last_send_ms_ = now_ms;
  return true;
}

uint64_t SignalOutbox::dropped() const {
  std::lock_guard<std::mutex> lock(m_);
  return dropped_;
}

size_t SignalOutbox::pending() const {
  std::lock_guard<std::mutex> lock(m_);
  return q_.size();
}

}  // namespace zc
