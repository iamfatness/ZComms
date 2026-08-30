// Channel background-volume planner (pure, no SDK includes, no windows.h).
//
// Two live-measured Zoom facts drive this (CoreVideo, same production,
// 2026-08-30): Zoom DUCKS a talkback channel member's meeting audio BY
// DEFAULT -- merely being placed in a channel reduces their meeting volume,
// and talent notices on assignment. SetChannelBackgroundVolume is a
// channel-scoped 0.0-2.0 gain (1.0 = unity), so one call covers late
// joiners. Policy enforced here: unity the moment a channel exists, duck
// only while that channel is actually keyed (transmitting).
//
// Shaped as a planner because every mutation is a rate-limited SDK call
// (code 18 -- the limiter is per CALL): the caller asks for at most one
// action per pass, reports the SDK's verdict back, and convergence is
// healed rather than assumed.
#pragma once

#include <cstdint>

namespace zc {

struct VolumeAction {
  int slot = -1;
  float volume = 1.0f;
};

class DuckPlanner {
 public:
  static constexpr float kUnity = 1.0f;  // cancel Zoom's default duck
  static constexpr float kDuck = 0.2f;   // duck, don't erase, the meeting
  static constexpr int64_t kPaceMs = 300;    // one SDK call per window
  static constexpr int64_t kRetryMs = 2000;  // after a refusal (code 18)

  // The one SetChannelBackgroundVolume call to make now, or false if
  // converged / paced out. Masks are slot bitmasks (bit i = slot i).
  bool Next(uint32_t ready_mask, uint32_t key_mask, int64_t now_ms,
            VolumeAction* out);
  void Confirm(const VolumeAction& a);  // Zoom accepted the call
  void Fail(int64_t now_ms);            // refused: back off, state unknown

 private:
  static constexpr int kSlots = 32;
  float applied_[kSlots] = {};
  bool known_[kSlots] = {};
  int64_t next_call_ms_ = 0;
};

}  // namespace zc
