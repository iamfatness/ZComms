// Per-channel TX composer: where voice and a latched extern feed meet.
//
// The routing arithmetic for one pacer tick, in exactly one place (spec
// docs/plans/2026-09-01-extern-feeds.md): a slot's outgoing frame is the
// operator's voice (when keyed) summed with that slot's latched feed, and
// while the voice is ACTUALLY carrying audio (SignalGate, not key state)
// the feed ducks to ~30% underneath it -- decision B's director barge,
// signal-gated per the ZoomISO refinement. The duck gain ramps rather than
// steps for the same reason PTT does: a step is a click.
//
// Pure: no SDK, no devices, no threads. The sink drives one Compose() per
// slot per tick on the pacer thread.
#pragma once

#include <cstdint>

namespace zc {

class ChannelMix {
 public:
  static constexpr int kSlots = 32;
  static constexpr float kBargeDuck = 0.3f;

  ChannelMix(double ramp_ms, int sample_rate);

  // One slot's frame for this tick. `voice` is the post-envelope operator
  // frame when this slot is keyed (null otherwise); `feed` is the slot's
  // latched feed frame (null when no feed is latched or its chain is fully
  // silent); `voice_active` is the voice SignalGate. Writes `samples` into
  // `out` and returns true, or returns false when the slot has nothing to
  // send this tick.
  bool Compose(int slot, const int16_t* voice, const int16_t* feed,
               bool voice_active, int samples, int16_t* out);

  float barge_gain(int slot) const { return gain_[slot]; }

 private:
  float gain_[kSlots];
  float step_ = 1.0f;
};

}  // namespace zc
