// Input gain and a look-ahead peak limiter.
//
// The limiter exists because this signal is going somewhere we do not control.
// Clipping on the way into Zoom's encoder is not recoverable downstream and
// sounds like distortion to everyone in the meeting, so the ceiling has to
// hold even when an operator sets input gain badly or coughs into a headset
// mic.
//
// Look-ahead is what makes the ceiling an actual guarantee rather than an
// aspiration. Without it, a limiter cannot begin reducing gain until a peak
// has already been output. The delay line here is short enough (a couple of
// milliseconds) to be irrelevant against Zoom's own transport, and it buys the
// gain reduction enough notice to be fully applied before the peak arrives.
//
// Gain reduction is driven by a sliding minimum over the look-ahead window, so
// the required gain is known in advance, then smoothed so the reduction itself
// does not become the transient it was meant to prevent.
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace zc {

// A gain control that ramps rather than steps. A raw multiplier changed
// between blocks produces a step at the block boundary -- zipper noise -- and
// operators do move faders while live.
class SmoothedGain {
 public:
  SmoothedGain(double initial_db, double smooth_ms, int sample_rate);
  void set_db(double db);
  double db() const { return target_db_; }
  void Process(float* buf, int n);

 private:
  double current_ = 1.0;
  double target_ = 1.0;
  double target_db_ = 0.0;
  double coeff_ = 0.0;
};

class Limiter {
 public:
  Limiter(double ceiling_dbfs, double lookahead_ms, double release_ms,
          int sample_rate);

  // In-place. Output is delayed by the look-ahead length; that delay is
  // constant and is reported by latency_samples().
  void Process(float* buf, int n);

  int latency_samples() const { return static_cast<int>(delay_.size()); }
  double ceiling() const { return ceiling_; }
  // Counts samples that needed reduction. A limiter that is always working is
  // an input-gain problem, and the operator should be told rather than left
  // wondering why they sound squashed.
  unsigned long long engaged_samples() const { return engaged_; }

 private:
  double RequiredGain(double sample_abs) const;

  double ceiling_;
  double release_coeff_;
  double attack_coeff_;
  double gain_ = 1.0;

  std::vector<float> delay_;
  size_t write_ = 0;

  // Monotonic deque holding indices into the look-ahead window, front always
  // the minimum required gain. O(1) amortised per sample.
  std::deque<std::pair<size_t, double>> mins_;
  size_t index_ = 0;

  unsigned long long engaged_ = 0;
};

}  // namespace zc
