#include "limiter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace zc {
namespace {

double DbToLinear(double db) { return std::pow(10.0, db / 20.0); }

// One-pole coefficient for a given time constant.
double Coeff(double ms, int sample_rate) {
  if (ms <= 0.0) return 0.0;
  return std::exp(-1.0 / (ms * 0.001 * sample_rate));
}

}  // namespace

// --- SmoothedGain -----------------------------------------------------------

SmoothedGain::SmoothedGain(double initial_db, double smooth_ms, int sample_rate)
    : current_(DbToLinear(initial_db)),
      target_(DbToLinear(initial_db)),
      target_db_(initial_db),
      coeff_(Coeff(smooth_ms, sample_rate)) {}

void SmoothedGain::set_db(double db) {
  target_db_ = db;
  target_ = DbToLinear(db);
}

void SmoothedGain::Process(float* buf, int n) {
  for (int i = 0; i < n; ++i) {
    current_ = target_ + (current_ - target_) * coeff_;
    buf[i] = static_cast<float>(buf[i] * current_);
  }
}

// --- Limiter ----------------------------------------------------------------

Limiter::Limiter(double ceiling_dbfs, double lookahead_ms, double release_ms,
                 int sample_rate)
    : ceiling_(DbToLinear(ceiling_dbfs)),
      release_coeff_(Coeff(release_ms, sample_rate)) {
  const int lookahead =
      std::max(1, static_cast<int>(lookahead_ms * sample_rate / 1000.0));
  delay_.assign(static_cast<size_t>(lookahead), 0.0f);

  // Attack time constant is a quarter of the look-ahead, so the reduction is
  // ~98% applied by the time the peak that demanded it reaches the output.
  // The residual is why the hard clamp below exists.
  attack_coeff_ = Coeff(lookahead_ms / 4.0, sample_rate);
}

double Limiter::RequiredGain(double sample_abs) const {
  if (sample_abs <= ceiling_) return 1.0;
  return ceiling_ / sample_abs;
}

void Limiter::Process(float* buf, int n) {
  const size_t L = delay_.size();
  for (int i = 0; i < n; ++i) {
    const float in = buf[i];
    const double required = RequiredGain(std::fabs(static_cast<double>(in)));

    // Monotonic deque: front is always the smallest required gain still
    // inside the look-ahead window. This is what lets the reduction start
    // before the peak rather than after it.
    while (!mins_.empty() && mins_.back().second >= required) mins_.pop_back();
    mins_.emplace_back(index_, required);
    while (!mins_.empty() && mins_.front().first + L <= index_) mins_.pop_front();

    const double target = mins_.front().second;

    // Down fast, up slowly. Releasing quickly would pump; attacking slowly
    // would let the peak through.
    const double coeff = (target < gain_) ? attack_coeff_ : release_coeff_;
    gain_ = target + (gain_ - target) * coeff;

    const float delayed = delay_[write_];
    delay_[write_] = in;
    write_ = (write_ + 1) % L;
    ++index_;

    double out = static_cast<double>(delayed) * gain_;

    // Final guarantee. The smoothing above leaves a small residual, and an
    // operator's clipped input is worth a hair of distortion here rather than
    // a full-scale sample handed to the encoder.
    if (out > ceiling_) {
      out = ceiling_;
    } else if (out < -ceiling_) {
      out = -ceiling_;
    }
    if (target < 1.0) ++engaged_;

    buf[i] = static_cast<float>(out);
  }
}

}  // namespace zc
