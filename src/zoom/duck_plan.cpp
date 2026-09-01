#include "duck_plan.h"

namespace zc {

bool DuckPlanner::Next(uint32_t ready_mask, uint32_t active_mask, int64_t now_ms,
                       VolumeAction* out) {
  if (now_ms < next_call_ms_) return false;
  for (int s = 0; s < kSlots; ++s) {
    if (((ready_mask >> s) & 1u) == 0) {
      known_[s] = false;  // gone; a re-created channel needs unity again
      continue;
    }
    const float want = ((active_mask >> s) & 1u) != 0 ? kDuck : kUnity;
    if (known_[s] && applied_[s] == want) continue;
    out->slot = s;
    out->volume = want;
    next_call_ms_ = now_ms + kPaceMs;
    return true;
  }
  return false;
}

void DuckPlanner::Confirm(const VolumeAction& a) {
  if (a.slot < 0 || a.slot >= kSlots) return;
  applied_[a.slot] = a.volume;
  known_[a.slot] = true;
}

void DuckPlanner::Fail(int64_t now_ms) {
  next_call_ms_ = now_ms + kRetryMs;
}

}  // namespace zc
