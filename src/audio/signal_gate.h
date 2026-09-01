// "Is actual audio flowing?" -- the detector both ducks are gated on.
//
// ZoomISO's field-proven behavior (owner, 2026-09-01): duck only when real
// audio moves down the pipe, never on mere key/latch state. A latched but
// silent extern feed must leave channel members' meeting audio at unity,
// and an idle held key must not dent a latched feed. So the meeting-audio
// DuckPlanner and the in-channel barge duck both take their cue from this
// gate rather than from control state.
//
// Time derives from sample counts, never wall clock (plan §5): the hang is
// measured in samples pushed through Update().
#pragma once

#include <cstdint>

namespace zc {

class SignalGate {
 public:
  // Instant attack; `hang_ms` keeps the gate open across speech gaps so a
  // duck does not flutter between words. -50 dBFS default threshold sits
  // well under speech but above line noise.
  SignalGate(double threshold_dbfs, int hang_ms, int sample_rate);

  // Push one frame; returns active(). A null frame (silence known without
  // a buffer) advances the hang clock by `samples`.
  bool Update(const int16_t* pcm, int samples);

  bool active() const { return hang_left_ > 0; }

 private:
  int threshold_ = 0;  // abs int16
  int64_t hang_samples_ = 0;
  int64_t hang_left_ = 0;
};

}  // namespace zc
