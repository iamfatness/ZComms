#include "stats.h"

#include <algorithm>
#include <cmath>

namespace zc {

double Percentile(const std::vector<double>& sorted, double pct) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return sorted[0];
  // Linear interpolation between order statistics. With ~150 samples the
  // difference from nearest-rank is small, but it stops p95 from stepping in
  // visible jumps as the sample count grows during a run.
  const double rank = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = static_cast<size_t>(std::ceil(rank));
  const double frac = rank - static_cast<double>(lo);
  return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

Summary Summarise(std::vector<double> samples) {
  Summary s;
  s.n = samples.size();
  if (samples.empty()) return s;

  std::sort(samples.begin(), samples.end());
  s.min_ms = samples.front();
  s.max_ms = samples.back();
  s.p50_ms = Percentile(samples, 50.0);
  s.p95_ms = Percentile(samples, 95.0);
  s.jitter_p95_p50_ms = s.p95_ms - s.p50_ms;

  double sum = 0.0;
  for (double v : samples) sum += v;
  s.mean_ms = sum / static_cast<double>(samples.size());

  double sq = 0.0;
  for (double v : samples) {
    const double d = v - s.mean_ms;
    sq += d * d;
  }
  s.stddev_ms = samples.size() > 1
                    ? std::sqrt(sq / static_cast<double>(samples.size() - 1))
                    : 0.0;

  std::vector<double> dev;
  dev.reserve(samples.size());
  for (double v : samples) dev.push_back(std::fabs(v - s.p50_ms));
  std::sort(dev.begin(), dev.end());
  s.mad_ms = Percentile(dev, 50.0);
  return s;
}

}  // namespace zc
