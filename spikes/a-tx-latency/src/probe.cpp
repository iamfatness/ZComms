#include "probe.h"

#include <algorithm>
#include <cmath>

#include "audio_defs.h"
#include "clock.h"

namespace zc {
namespace {
// Retained capture history. Must comfortably exceed max_latency plus the
// resolve grace, or a burst's own audio would age out before we looked for it.
constexpr double kCaptureHistorySeconds = 6.0;
}  // namespace

LatencyProbe::LatencyProbe(const std::vector<float>* burst_up,
                           const std::vector<float>* burst_down,
                           const ProbeConfig& cfg)
    : up_(burst_up), down_(burst_down), cfg_(cfg) {
  capture_.reserve(
      static_cast<size_t>(kCaptureHistorySeconds * kSampleRate * 1.5));
}

void LatencyProbe::OnEmission(int32_t burst_id, bool up, int64_t host_ns) {
  std::lock_guard<std::mutex> lock(m_);
  pending_.push_back({burst_id, up, host_ns});
  ++stats_.emitted;
}

void LatencyProbe::OnCapture(const float* mono, int count, int64_t host_ns) {
  if (count <= 0) return;
  std::lock_guard<std::mutex> lock(m_);

  capture_.insert(capture_.end(), mono, mono + count);
  capture_total_ += static_cast<uint64_t>(count);

  // The anchor is (cumulative index at the end of this buffer, host time now).
  // The audio at that index is the most recently captured audio, so the pair
  // is the tightest index/time correspondence the callback can offer.
  timebase_.AddAnchor(capture_total_, host_ns);

  double sum_sq = 0.0;
  for (int i = 0; i < count; ++i) sum_sq += static_cast<double>(mono[i]) * mono[i];
  const double rms = std::sqrt(sum_sq / count);
  // Slow-moving average, purely diagnostic: it is what tells a failed run
  // apart from a silent one.
  stats_.capture_rms = stats_.capture_rms * 0.95 + rms * 0.05;

  const size_t max_hold = static_cast<size_t>(kCaptureHistorySeconds * kSampleRate);
  if (capture_.size() > max_hold) {
    const size_t drop = capture_.size() - max_hold;
    capture_.erase(capture_.begin(),
                   capture_.begin() + static_cast<std::ptrdiff_t>(drop));
    capture_base_index_ += drop;
  }
}

void LatencyProbe::Resolve() {
  std::lock_guard<std::mutex> lock(m_);
  if (pending_.empty()) return;

  double rate = 0.0;
  if (timebase_.MeasuredRateHz(&rate)) stats_.measured_capture_rate_hz = rate;

  const int64_t now_ns = NowNs();
  const uint64_t newest = capture_base_index_ + capture_.size();

  std::vector<Pending> still_pending;
  still_pending.reserve(pending_.size());

  for (const Pending& p : pending_) {
    const int64_t win_lo_ns =
        p.emit_ns + static_cast<int64_t>(cfg_.min_latency_ms * 1e6);
    const int64_t win_hi_ns =
        p.emit_ns + static_cast<int64_t>(cfg_.max_latency_ms * 1e6);
    const int64_t give_up_ns =
        win_hi_ns + static_cast<int64_t>(cfg_.resolve_grace_ms * 1e6);

    double f_lo_d = 0.0, f_hi_d = 0.0;
    if (!timebase_.FrameForHostNs(win_lo_ns, &f_lo_d) ||
        !timebase_.FrameForHostNs(win_hi_ns, &f_hi_d)) {
      // Timebase not fitted yet. Hold unless it is long past due.
      if (now_ns < give_up_ns) {
        still_pending.push_back(p);
      } else {
        ++stats_.data_gap;
      }
      continue;
    }

    const std::vector<float>& needle = p.up ? *up_ : *down_;
    const int64_t f_lo = static_cast<int64_t>(std::floor(f_lo_d));
    // The slice has to hold the whole burst even if it arrived at the very end
    // of the window, hence the needle length on top of the window itself.
    const int64_t f_hi =
        static_cast<int64_t>(std::ceil(f_hi_d)) +
        static_cast<int64_t>(needle.size());

    if (f_lo < 0) {
      ++stats_.data_gap;
      continue;
    }
    if (static_cast<uint64_t>(f_hi) > newest) {
      // The audio that would contain this burst has not arrived yet.
      if (now_ns < give_up_ns) {
        still_pending.push_back(p);
      } else {
        ++stats_.data_gap;
      }
      continue;
    }
    if (static_cast<uint64_t>(f_lo) < capture_base_index_) {
      // Trimmed before we got to it. Counted separately from a failed match:
      // one means the harness fell behind, the other means Zoom did not
      // deliver a recognisable burst, and conflating them would hide whichever
      // is actually happening.
      ++stats_.data_gap;
      continue;
    }

    const size_t off = static_cast<size_t>(
        static_cast<uint64_t>(f_lo) - capture_base_index_);
    const size_t len = static_cast<size_t>(f_hi - f_lo);
    if (off + len > capture_.size()) {
      ++stats_.data_gap;
      continue;
    }

    std::vector<float> slice(capture_.begin() + static_cast<std::ptrdiff_t>(off),
                             capture_.begin() +
                                 static_cast<std::ptrdiff_t>(off + len));
    const Detection det = FindBurst(slice, needle, cfg_.detector);
    if (!det.found) {
      ++stats_.no_detection;
      if (det.peak > stats_.best_failed_peak) {
        stats_.best_failed_peak = det.peak;
        stats_.best_failed_psr = det.psr;
      }
      continue;
    }

    const double hit_frame = static_cast<double>(f_lo) + det.lag_samples;
    int64_t recv_ns = 0;
    if (!timebase_.HostNsForFrame(hit_frame, &recv_ns)) {
      ++stats_.data_gap;
      continue;
    }

    LatencySample s;
    s.burst_id = p.burst_id;
    s.latency_ms = NsToMs(recv_ns - p.emit_ns);
    s.peak = det.peak;
    s.psr = det.psr;
    samples_.push_back(s);
    ++stats_.resolved;
  }

  pending_.swap(still_pending);
  stats_.pending = pending_.size();
}

std::vector<LatencySample> LatencyProbe::samples() const {
  std::lock_guard<std::mutex> lock(m_);
  return samples_;
}

ProbeStats LatencyProbe::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  ProbeStats s = stats_;
  s.pending = pending_.size();
  return s;
}

}  // namespace zc
