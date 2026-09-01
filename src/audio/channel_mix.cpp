#include "channel_mix.h"

namespace zc {

ChannelMix::ChannelMix(double ramp_ms, int sample_rate) {
  for (int i = 0; i < kSlots; ++i) gain_[i] = 1.0f;
  const double travel = ramp_ms > 0.0 ? ramp_ms * sample_rate / 1000.0 : 1.0;
  step_ = static_cast<float>(1.0 / travel);  // full 0->1 in ramp_ms
}

bool ChannelMix::Compose(int slot, const int16_t* voice, const int16_t* feed,
                         bool voice_active, int samples, int16_t* out) {
  if (slot < 0 || slot >= kSlots) return false;
  if (voice == nullptr && feed == nullptr) return false;

  // The barge duck engages only when there is a feed to duck AND the voice
  // is genuinely flowing on this slot. A keyed-but-silent operator leaves
  // the feed untouched -- that is the whole ZoomISO refinement.
  const float target =
      (feed != nullptr && voice != nullptr && voice_active) ? kBargeDuck : 1.0f;

  float g = gain_[slot];
  for (int i = 0; i < samples; ++i) {
    if (g < target) {
      g += step_;
      if (g > target) g = target;
    } else if (g > target) {
      g -= step_;
      if (g < target) g = target;
    }
    float acc = 0.0f;
    if (voice != nullptr) acc += static_cast<float>(voice[i]);
    if (feed != nullptr) acc += static_cast<float>(feed[i]) * g;
    if (acc > 32767.0f) acc = 32767.0f;
    if (acc < -32768.0f) acc = -32768.0f;
    out[i] = static_cast<int16_t>(acc);
  }
  gain_[slot] = g;
  return true;
}

}  // namespace zc
