// The probe signal: a ramped chirp burst on a low comfort-noise bed.
//
// Three constraints shaped this, and none of them are aesthetic:
//
//   Chirp, not a tone. A 40 ms sweep has a sharp autocorrelation peak; a
//   continuous tone correlates with every one of its own periods and gives no
//   unambiguous arrival time at all.
//
//   Ramped edges. Plan §5: Zoom's audio path makes a hard amplitude edge
//   audible as a click, and a PTT release is deliberately that same
//   transition. The window here is the same shape the real PTT will use, so
//   the harness seeds §6.1 rather than teaching a habit that has to be undone.
//
//   A comfort-noise bed between bursts. Zoom runs VAD/DTX on the way in. An
//   isolated burst after a long silence is exactly what a noise gate is built
//   to discard, and a gated burst reads as "no signal" rather than as "high
//   latency" -- it would look like a measurement failure, not a product one.
//   Holding a low bed open keeps the encoder streaming, which is also what
//   plan §6.1 wants from the virtual mic anyway.
#pragma once

#include <cstdint>
#include <vector>

namespace zc {

struct SignalParams {
  double burst_ms = 40.0;      // exactly 2 TX frames
  double ramp_ms = 8.0;        // raised-cosine edges, per plan §5
  double f_start_hz = 500.0;   // inside the band a speech codec preserves
  double f_end_hz = 3500.0;
  double burst_dbfs = -12.0;
  double bed_dbfs = -45.0;     // comfort noise: holds VAD open, stays inaudible
  double period_ms = 2000.0;   // burst cadence
};

// One burst waveform, float in [-1,1]. `up` selects an ascending or descending
// sweep: consecutive bursts alternate so that a detection can never be matched
// against the wrong emission even if the search window is widened later.
std::vector<float> MakeBurst(const SignalParams& p, bool up);

// Deterministic low-level noise bed. Deterministic matters: the bed is
// subtracted from nothing and correlated against nothing, but a reproducible
// signal makes a failed run replayable.
class NoiseBed {
 public:
  explicit NoiseBed(double dbfs, uint32_t seed = 0x5EEDu);
  void Fill(float* out, int count);

 private:
  uint32_t state_;
  float amplitude_;
};

int16_t FloatToPcm16(float v);

}  // namespace zc
