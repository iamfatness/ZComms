// Paced chat send queue (pure, no SDK includes, no windows.h).
//
// Zoom rate-limits back-to-back SDK calls (SDKERR_TOO_FREQUENT_CALL, 18 --
// hit live on talkback invites 2026-08-29), so chat sends queue here and
// leave at most one per 300 ms. FIFO, capped at 64 with drop-oldest and a
// counted drop stat: a cue that cannot be delivered promptly is stale
// anyway, and a blocking send on the SDK pump thread is never acceptable.
//
// The clock is caller-supplied monotonic milliseconds (same injection
// pattern as TxPacer), which is what makes this testable to the exact
// millisecond with no waiting.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace zc {

struct OutboundChat {
  unsigned int receiver_user_id = 0;  // 0 = to all
  std::string content;
};

class SignalOutbox {
 public:
  static constexpr int kMinSendGapMs = 300;
  static constexpr size_t kMaxQueued = 64;

  // Push may run on a panel action thread; PopReady runs on the pump thread.
  void Push(const OutboundChat& msg);
  // Returns true and fills *out when now_ms is past the pacing gate.
  bool PopReady(int64_t now_ms, OutboundChat* out);
  uint64_t dropped() const;
  size_t pending() const;

 private:
  mutable std::mutex m_;
  std::deque<OutboundChat> q_;
  int64_t last_send_ms_ = INT64_MIN;
  uint64_t dropped_ = 0;
};

}  // namespace zc
