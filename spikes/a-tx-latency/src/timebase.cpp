#include "timebase.h"

#include <cmath>

namespace zc {

CaptureTimebase::CaptureTimebase(size_t window)
    : window_(window < kMinAnchors ? kMinAnchors : window) {}

void CaptureTimebase::AddAnchor(uint64_t frame_index_at_end, int64_t host_ns) {
  anchors_.push_back({static_cast<double>(frame_index_at_end),
                      static_cast<double>(host_ns)});
  while (anchors_.size() > window_) anchors_.pop_front();
}

bool CaptureTimebase::Fit(double* slope, double* intercept, double* frame_mean,
                          double* host_mean) const {
  if (anchors_.size() < kMinAnchors) return false;

  // Centre both axes before fitting. Frame indices reach tens of millions and
  // host_ns is a full nanosecond epoch; forming the raw sums of squares would
  // lose most of the mantissa to cancellation.
  double fm = 0.0, hm = 0.0;
  for (const Anchor& a : anchors_) {
    fm += a.frame_index;
    hm += a.host_ns;
  }
  const double n = static_cast<double>(anchors_.size());
  fm /= n;
  hm /= n;

  double sxy = 0.0, sxx = 0.0;
  for (const Anchor& a : anchors_) {
    const double dx = a.frame_index - fm;
    const double dy = a.host_ns - hm;
    sxy += dx * dy;
    sxx += dx * dx;
  }
  if (sxx <= 0.0) return false;  // every anchor at the same index

  *slope = sxy / sxx;
  *intercept = hm - (*slope) * fm;
  *frame_mean = fm;
  *host_mean = hm;
  return true;
}

bool CaptureTimebase::HostNsForFrame(double frame_index, int64_t* out_ns) const {
  double slope = 0.0, intercept = 0.0, fm = 0.0, hm = 0.0;
  if (!Fit(&slope, &intercept, &fm, &hm)) return false;
  // Evaluated in centred form for the same conditioning reason as the fit.
  const double ns = hm + slope * (frame_index - fm);
  *out_ns = static_cast<int64_t>(std::llround(ns));
  return true;
}

bool CaptureTimebase::FrameForHostNs(int64_t host_ns, double* out_frame) const {
  double slope = 0.0, intercept = 0.0, fm = 0.0, hm = 0.0;
  if (!Fit(&slope, &intercept, &fm, &hm)) return false;
  if (slope <= 0.0) return false;
  *out_frame = fm + (static_cast<double>(host_ns) - hm) / slope;
  return true;
}

bool CaptureTimebase::MeasuredRateHz(double* out_hz) const {
  double slope = 0.0, intercept = 0.0, fm = 0.0, hm = 0.0;
  if (!Fit(&slope, &intercept, &fm, &hm)) return false;
  if (slope <= 0.0) return false;
  *out_hz = 1e9 / slope;  // slope is ns per frame
  return true;
}

}  // namespace zc
