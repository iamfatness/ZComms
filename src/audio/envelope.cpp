#include "envelope.h"

#include <algorithm>
#include <cmath>

namespace zc {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Raised cosine over the linear position. Zero slope at both ends is the
// whole point -- see the header.
inline double Shape(double pos) { return 0.5 * (1.0 - std::cos(kPi * pos)); }
}  // namespace

Envelope::Envelope(double fade_ms, int sample_rate) {
  const double samples = std::max(1.0, fade_ms * sample_rate / 1000.0);
  step_ = 1.0 / samples;
}

void Envelope::Open() { target_ = 1.0; }
void Envelope::Close() { target_ = 0.0; }

float Envelope::gain() const { return static_cast<float>(Shape(pos_)); }

void Envelope::Step() {
  // Moves toward the target from wherever it currently is. A press during a
  // release ramp reverses from the current position rather than restarting,
  // which is what keeps a fast double-tap continuous.
  if (pos_ < target_) {
    pos_ = std::min(target_, pos_ + step_);
  } else if (pos_ > target_) {
    pos_ = std::max(target_, pos_ - step_);
  }
}

void Envelope::Process(float* buf, int n) {
  for (int i = 0; i < n; ++i) {
    buf[i] = static_cast<float>(buf[i] * Shape(pos_));
    Step();
  }
}

void Envelope::Advance(int n) {
  for (int i = 0; i < n; ++i) Step();
}

}  // namespace zc
