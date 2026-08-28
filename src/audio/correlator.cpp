#include "correlator.h"

#include <algorithm>
#include <cmath>

#include "audio_defs.h"

namespace zc {

Detection FindBurst(const std::vector<float>& haystack,
                    const std::vector<float>& needle,
                    const DetectorConfig& cfg) {
  Detection d;
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(needle.size());
  const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(haystack.size());
  if (n == 0 || m < n) return d;

  double needle_energy = 0.0;
  for (float v : needle) needle_energy += static_cast<double>(v) * v;
  if (needle_energy <= 0.0) return d;
  const double needle_norm = std::sqrt(needle_energy);

  const std::ptrdiff_t lags = m - n + 1;
  std::vector<double> corr(static_cast<size_t>(lags));

  // Running window energy: recomputing sum(x^2) per lag would make this
  // O(lags*n) for the denominator alone, on top of the numerator.
  double window_energy = 0.0;
  for (std::ptrdiff_t i = 0; i < n; ++i) {
    window_energy += static_cast<double>(haystack[static_cast<size_t>(i)]) *
                     haystack[static_cast<size_t>(i)];
  }

  for (std::ptrdiff_t lag = 0; lag < lags; ++lag) {
    double acc = 0.0;
    const float* h = haystack.data() + lag;
    const float* p = needle.data();
    for (std::ptrdiff_t i = 0; i < n; ++i) {
      acc += static_cast<double>(h[i]) * p[i];
    }
    const double denom = needle_norm * std::sqrt(window_energy);
    corr[static_cast<size_t>(lag)] = denom > 0.0 ? std::fabs(acc / denom) : 0.0;

    // Slide the energy window forward one sample.
    if (lag + n < m) {
      const double out_v = haystack[static_cast<size_t>(lag)];
      const double in_v = haystack[static_cast<size_t>(lag + n)];
      window_energy += in_v * in_v - out_v * out_v;
      if (window_energy < 0.0) window_energy = 0.0;  // guard FP drift
    }
  }

  const auto peak_it = std::max_element(corr.begin(), corr.end());
  const std::ptrdiff_t peak_idx = std::distance(corr.begin(), peak_it);
  d.peak = *peak_it;

  const std::ptrdiff_t guard =
      static_cast<std::ptrdiff_t>(cfg.guard_ms * kSampleRate / 1000.0);
  double sidelobe = 0.0;
  for (std::ptrdiff_t i = 0; i < lags; ++i) {
    if (std::llabs(static_cast<long long>(i - peak_idx)) <= guard) continue;
    sidelobe = std::max(sidelobe, corr[static_cast<size_t>(i)]);
  }
  // No sidelobe at all means the search range was barely wider than the
  // needle. Report a finite ratio rather than infinity so the gate stays
  // meaningful and the value stays printable.
  d.psr = sidelobe > 1e-12 ? d.peak / sidelobe : (d.peak > 0.0 ? 999.0 : 0.0);

  // Parabolic interpolation through the three samples around the peak. The
  // correlation of a 3 kHz-bandwidth chirp has a main lobe well under a
  // sample wide, so integer-sample resolution alone would quantise every
  // measurement to 20.8 us for no reason.
  double offset = 0.0;
  if (peak_idx > 0 && peak_idx + 1 < lags) {
    const double y0 = corr[static_cast<size_t>(peak_idx - 1)];
    const double y1 = corr[static_cast<size_t>(peak_idx)];
    const double y2 = corr[static_cast<size_t>(peak_idx + 1)];
    const double denom = y0 - 2.0 * y1 + y2;
    if (std::fabs(denom) > 1e-12) {
      offset = 0.5 * (y0 - y2) / denom;
      if (offset < -1.0 || offset > 1.0) offset = 0.0;  // reject a bad fit
    }
  }

  d.lag_samples = static_cast<double>(peak_idx) + offset;
  d.found = d.peak >= cfg.min_peak && d.psr >= cfg.min_psr;
  return d;
}

}  // namespace zc
