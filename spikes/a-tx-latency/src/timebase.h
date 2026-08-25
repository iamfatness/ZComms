// Maps a capture-stream sample index onto the host clock.
//
// This is the piece the whole measurement stands on, and it is the piece the
// brief warns about: two wall clocks is not a measurement. There is exactly
// one clock here (steady_clock, QPC-backed on Windows). The emission side
// reads it directly. The capture side cannot -- audio arrives in buffers whose
// delivery time jitters by milliseconds -- so the capture side is *mapped*
// onto that same clock instead of being given a clock of its own.
//
// The map is a least-squares fit of host time against cumulative sample index
// over a sliding window, which handles the two error sources differently
// because they are different:
//
//   Callback jitter is zero-mean noise on each anchor. Fitting a line through
//   many anchors averages it out; using the single most recent anchor would
//   inherit all of it.
//
//   Sample-clock drift is not noise. The capture device's crystal is not
//   48000.000 Hz and is not disciplined to QPC, so index-to-time has a slope
//   that is genuinely not nominal. Over a five-minute run even 100 ppm is
//   30 ms of accumulated error -- an eighth of the kill threshold -- if you
//   assume the nominal rate. The fit measures the real slope, and keeping the
//   window short keeps it tracking a slope that itself moves.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace zc {

class CaptureTimebase {
 public:
  // `window` is the number of anchors retained. At a typical 10 ms capture
  // period, 200 anchors is a ~2 s regression window: long enough to average
  // the jitter down, short enough that a drifting slope is still local.
  explicit CaptureTimebase(size_t window = 200);

  // Called once per capture callback. `frame_index_at_end` is the cumulative
  // count of frames captured including this buffer; `host_ns` is steady_clock
  // read at the top of the callback.
  void AddAnchor(uint64_t frame_index_at_end, int64_t host_ns);

  bool Ready() const { return anchors_.size() >= kMinAnchors; }

  // Host time for an arbitrary sample index, including indices inside the
  // window (interpolation) and slightly beyond it (short extrapolation).
  bool HostNsForFrame(double frame_index, int64_t* out_ns) const;

  // The inverse: which sample index corresponds to a given host time. Used to
  // turn "search the audio that arrived between 20 ms and 1200 ms after this
  // burst was emitted" into a slice of the capture buffer.
  bool FrameForHostNs(int64_t host_ns, double* out_frame) const;

  // Measured sample rate in Hz from the fitted slope. Nominal is 48000; the
  // gap is the drift being corrected for, and it is worth printing because a
  // wildly wrong value means the capture stream is not what we think it is.
  bool MeasuredRateHz(double* out_hz) const;

  size_t anchor_count() const { return anchors_.size(); }

 private:
  struct Anchor {
    double frame_index;
    double host_ns;
  };

  static constexpr size_t kMinAnchors = 8;

  bool Fit(double* slope_ns_per_frame, double* intercept_ns,
           double* frame_mean, double* host_mean) const;

  std::deque<Anchor> anchors_;
  size_t window_;
};

}  // namespace zc
