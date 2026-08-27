// PTT gain envelope.
//
// Plan §5, and the reason it is a load-bearing constraint rather than polish:
// Zoom's audio path makes a hard amplitude edge audible as a click, and a PTT
// release is deliberately exactly that transition -- full amplitude to zero,
// on an operator action, potentially several times a minute. Gating would put
// a click on every press and every release of the product's single most-used
// control.
//
// So the envelope ramps, and it ramps on a raised cosine rather than a
// straight line. A linear fade is continuous in value but not in slope; the
// corner at each end is itself a (weaker) click. Raised cosine has zero
// derivative at both endpoints, so the signal eases in and out.
//
// The other property that matters is that a reversal mid-ramp stays
// continuous. An operator who taps PTT twice quickly, or releases before the
// ramp finished, must not produce a jump -- so the state is a position along
// the ramp that reverses in place, never a restart from an endpoint.
#pragma once

namespace zc {

class Envelope {
 public:
  // `fade_ms` is the full 0->1 travel time. 10-15 ms is inaudible as a fade
  // while still being fast enough that an operator does not perceive the
  // press as laggy.
  Envelope(double fade_ms, int sample_rate);

  void Open();   // PTT pressed
  void Close();  // PTT released

  bool open() const { return target_ > 0.5; }
  // True once fully closed. Callers use this to know when it is safe to stop
  // mixing a source rather than guessing from the button state.
  bool silent() const { return pos_ <= 0.0 && target_ <= 0.0; }

  float gain() const;

  // Applies the envelope in place, advancing one step per sample.
  void Process(float* buf, int n);
  // Advances without touching audio, for a path that is muted anyway.
  void Advance(int n);

 private:
  void Step();

  double pos_ = 0.0;     // linear position along the ramp, 0..1
  double target_ = 0.0;  // 0 or 1
  double step_ = 1.0;    // per-sample increment
};

}  // namespace zc
