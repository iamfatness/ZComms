#include <cmath>
#include <random>
#include <vector>

#include "audio_defs.h"
#include "correlator.h"
#include "signal.h"
#include "test_util.h"

using namespace zc;

namespace {

// Builds noise with a burst planted at `offset`, scaled by `gain` and
// optionally polarity-inverted -- the two things a codec round trip through
// Zoom is most likely to do to the waveform.
std::vector<float> Haystack(const std::vector<float>& burst, int offset,
                            int total, float noise_amp, float gain,
                            unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> h(static_cast<size_t>(total));
  for (auto& v : h) v = dist(rng) * noise_amp;
  for (size_t i = 0; i < burst.size(); ++i) {
    const size_t idx = static_cast<size_t>(offset) + i;
    if (idx < h.size()) h[idx] += burst[i] * gain;
  }
  return h;
}

}  // namespace

void TestCorrelator() {
  std::printf("TestCorrelator\n");
  SignalParams p;
  const std::vector<float> up = MakeBurst(p, true);
  const std::vector<float> down = MakeBurst(p, false);
  DetectorConfig cfg;

  {
    ZC_TEST("recovers a clean burst at a known offset");
    const int offset = 5000;
    auto h = Haystack(up, offset, 20000, 0.0f, 1.0f, 1);
    const Detection d = FindBurst(h, up, cfg);
    ZC_CHECK(d.found);
    ZC_CHECK_NEAR(d.lag_samples, offset, 1.0);
  }

  {
    ZC_TEST("survives noise at roughly the burst's own level");
    // Burst peak is -12 dBFS; noise at 0.05 amplitude is comparable, so this
    // is a far worse SNR than a real Zoom leg should ever deliver.
    const int offset = 9137;
    auto h = Haystack(up, offset, 40000, 0.05f, 1.0f, 2);
    const Detection d = FindBurst(h, up, cfg);
    ZC_CHECK(d.found);
    ZC_CHECK_NEAR(d.lag_samples, offset, 2.0);
  }

  {
    ZC_TEST("is amplitude invariant (AGC on the path)");
    const int offset = 3000;
    for (float gain : {0.05f, 0.3f, 1.0f, 3.0f}) {
      auto h = Haystack(up, offset, 20000, 0.001f, gain, 3);
      const Detection d = FindBurst(h, up, cfg);
      ZC_CHECK(d.found);
      ZC_CHECK_NEAR(d.lag_samples, offset, 2.0);
    }
  }

  {
    ZC_TEST("tolerates polarity inversion");
    const int offset = 7000;
    auto h = Haystack(up, offset, 20000, 0.001f, -1.0f, 4);
    const Detection d = FindBurst(h, up, cfg);
    ZC_CHECK(d.found);
    ZC_CHECK_NEAR(d.lag_samples, offset, 2.0);
  }

  {
    ZC_TEST("rejects pure noise rather than inventing a peak");
    // The property that matters most: a false detection would become a
    // fabricated latency sample and quietly corrupt the distribution.
    auto h = Haystack(up, 0, 40000, 0.05f, 0.0f, 5);
    const Detection d = FindBurst(h, up, cfg);
    ZC_CHECK(!d.found);
  }

  {
    ZC_TEST("does not confuse an up-chirp for a down-chirp");
    // This is what makes consecutive bursts unambiguous.
    const int offset = 6000;
    auto h = Haystack(up, offset, 20000, 0.001f, 1.0f, 6);
    const Detection d = FindBurst(h, down, cfg);
    ZC_CHECK(!d.found);
  }

  {
    ZC_TEST("sub-sample interpolation stays within one sample");
    for (int offset : {1000, 1001, 1002, 1003}) {
      auto h = Haystack(up, offset, 20000, 0.001f, 1.0f, 7);
      const Detection d = FindBurst(h, up, cfg);
      ZC_CHECK(d.found);
      ZC_CHECK_NEAR(d.lag_samples, offset, 1.0);
    }
  }
}
