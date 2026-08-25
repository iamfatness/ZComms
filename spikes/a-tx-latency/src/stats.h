// Distribution summary for the latency samples.
//
// A single number is not a result here. Plan §9's kill criterion is about what
// live crew experience, and an intercom whose median is fine but whose p95 is
// 400 ms is not fine -- the tail is what an operator hears as the thing being
// broken. So median and p95 are both first-class, and the spread between them
// is reported as the jitter figure rather than left to the reader.
#pragma once

#include <cstddef>
#include <vector>

namespace zc {

struct Summary {
  size_t n = 0;
  double min_ms = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double max_ms = 0.0;
  double mean_ms = 0.0;
  double stddev_ms = 0.0;
  // Median absolute deviation: a spread figure that a handful of outliers
  // cannot inflate, unlike stddev.
  double mad_ms = 0.0;
  // p95 - p50. The number that decides whether the tail is the problem.
  double jitter_p95_p50_ms = 0.0;
};

// `samples` is taken by value: it has to be sorted, and sorting the caller's
// vector out from under it would be a surprise.
Summary Summarise(std::vector<double> samples);

double Percentile(const std::vector<double>& sorted, double pct);

}  // namespace zc
