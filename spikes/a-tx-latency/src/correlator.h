// Matched-filter search for a burst inside captured audio.
//
// Normalised so the result is amplitude-invariant: Zoom applies AGC on the way
// through and the far client applies its own output gain, so an absolute
// threshold on correlation energy would drift with volume settings. The
// normalised peak is a shape match and does not care.
//
// The peak is taken on |corr| rather than corr. A codec round trip can invert
// polarity, and a detector that only accepts a positive peak would silently
// miss every burst if it did.
#pragma once

#include <cstddef>
#include <vector>

namespace zc {

struct Detection {
  bool found = false;
  double lag_samples = 0.0;  // sub-sample, relative to the start of `haystack`
  double peak = 0.0;         // normalised |correlation| at the peak, 0..1
  double psr = 0.0;          // peak-to-sidelobe ratio -- the confidence metric
};

struct DetectorConfig {
  // A burst that survives Zoom's codec still correlates well above this; the
  // gate exists to reject a window that contains only the comfort-noise bed.
  double min_peak = 0.15;
  // The discriminator that matters. A true match towers over its own
  // sidelobes; a spurious match on noise does not.
  double min_psr = 3.0;
  double guard_ms = 2.0;  // sidelobe search excludes +/- this around the peak
};

// Slides `needle` over `haystack` and returns the best match. Searches lags in
// [0, haystack.size() - needle.size()].
Detection FindBurst(const std::vector<float>& haystack,
                    const std::vector<float>& needle,
                    const DetectorConfig& cfg);

}  // namespace zc
