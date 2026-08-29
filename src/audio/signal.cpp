#include "signal.h"

#include <algorithm>
#include <cmath>

#include "audio_defs.h"

namespace zc {
namespace {
constexpr double kPi = 3.14159265358979323846;

double DbToLinear(double dbfs) { return std::pow(10.0, dbfs / 20.0); }
}  // namespace

std::vector<float> MakeBurst(const SignalParams& p, bool up) {
  const int n = static_cast<int>(p.burst_ms * kSampleRate / 1000.0);
  const int ramp = std::min(static_cast<int>(p.ramp_ms * kSampleRate / 1000.0), n / 2);
  const double amp = DbToLinear(p.burst_dbfs);
  const double duration_s = static_cast<double>(n) / kSampleRate;

  const double f0 = up ? p.f_start_hz : p.f_end_hz;
  const double f1 = up ? p.f_end_hz : p.f_start_hz;
  const double rate = (f1 - f0) / duration_s;  // Hz per second

  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    // Instantaneous phase of a linear sweep is the integral of the
    // instantaneous frequency, hence the t^2 term.
    const double phase = 2.0 * kPi * (f0 * t + 0.5 * rate * t * t);

    // Tukey window: raised-cosine in, flat, raised-cosine out. Flat in the
    // middle keeps most of the burst's energy, which the correlation peak
    // needs; the cosine edges are what keep it from clicking.
    double w = 1.0;
    if (ramp > 0) {
      if (i < ramp) {
        w = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(i) / ramp));
      } else if (i >= n - ramp) {
        const int k = n - 1 - i;
        w = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(k) / ramp));
      }
    }
    out[static_cast<size_t>(i)] = static_cast<float>(amp * w * std::sin(phase));
  }
  return out;
}

NoiseBed::NoiseBed(double dbfs, uint32_t seed)
    : state_(seed ? seed : 1u), amplitude_(static_cast<float>(DbToLinear(dbfs))) {}

void NoiseBed::Fill(float* out, int count) {
  for (int i = 0; i < count; ++i) {
    // xorshift32: cheap, deterministic, and good enough for a dither-level bed.
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    const float u = (static_cast<float>(state_) / 4294967296.0f) * 2.0f - 1.0f;
    out[i] = u * amplitude_;
  }
}

int16_t FloatToPcm16(float v) {
  const float clamped = std::max(-1.0f, std::min(1.0f, v));
  // 32767 not 32768: scaling by 32768 lets -1.0 map correctly but +1.0 wrap.
  return static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
}

}  // namespace zc
