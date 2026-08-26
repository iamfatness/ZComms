#include <cmath>
#include <vector>

#include "audio_defs.h"
#include "limiter.h"
#include "test_util.h"

using namespace zc;

namespace {
double DbToLin(double db) { return std::pow(10.0, db / 20.0); }

std::vector<float> Sine(int n, double hz, double amp) {
  std::vector<float> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    v[static_cast<size_t>(i)] = static_cast<float>(
        amp * std::sin(2.0 * 3.14159265358979 * hz * i / kSampleRate));
  }
  return v;
}

double Peak(const std::vector<float>& v) {
  double p = 0.0;
  for (float s : v) p = std::max(p, std::fabs(static_cast<double>(s)));
  return p;
}
}  // namespace

void TestLimiter() {
  std::printf("TestLimiter\n");

  {
    ZC_TEST("holds the ceiling on a signal well over it");
    // The property the whole thing exists for: clipping into Zoom's encoder is
    // not recoverable downstream.
    Limiter lim(-1.0, 2.0, 80.0, kSampleRate);
    auto buf = Sine(48000, 440.0, 4.0);  // +12 dB over full scale
    lim.Process(buf.data(), static_cast<int>(buf.size()));
    ZC_CHECK(Peak(buf) <= DbToLin(-1.0) + 1e-6);
    ZC_CHECK(lim.engaged_samples() > 0);
  }

  {
    ZC_TEST("holds the ceiling against a sudden transient");
    // Look-ahead is what makes this pass. Without it the first peak of a step
    // is already out of the door before any reduction can apply.
    Limiter lim(-1.0, 2.0, 80.0, kSampleRate);
    std::vector<float> buf(static_cast<size_t>(4800), 0.0f);
    for (size_t i = 2400; i < buf.size(); ++i) buf[i] = 0.99f;
    lim.Process(buf.data(), static_cast<int>(buf.size()));
    ZC_CHECK(Peak(buf) <= DbToLin(-1.0) + 1e-6);
  }

  {
    ZC_TEST("is transparent below the ceiling");
    // A limiter that quietly attenuates everything would cost level for no
    // reason and would be invisible until someone measured it.
    Limiter lim(-1.0, 2.0, 80.0, kSampleRate);
    auto buf = Sine(24000, 440.0, 0.25);
    const auto original = buf;
    lim.Process(buf.data(), static_cast<int>(buf.size()));

    const int lat = lim.latency_samples();
    ZC_CHECK(lat > 0);
    // Compare past the look-ahead delay, allowing for it.
    for (size_t i = 4800; i < buf.size(); ++i) {
      ZC_CHECK_NEAR(buf[i], original[i - static_cast<size_t>(lat)], 1e-4);
    }
    ZC_CHECK(lim.engaged_samples() == 0);
  }

  {
    ZC_TEST("reports a constant, non-zero latency");
    // Callers need this to be knowable, not guessable -- it is part of the
    // end-to-end budget the product is sold on.
    Limiter lim(-1.0, 2.0, 80.0, kSampleRate);
    const int expected = static_cast<int>(2.0 * kSampleRate / 1000.0);
    ZC_CHECK(lim.latency_samples() == expected);
  }

  {
    ZC_TEST("smoothed gain does not step between blocks");
    // Zipper noise: operators do move faders while live.
    SmoothedGain g(0.0, 15.0, kSampleRate);
    std::vector<float> buf(1000, 1.0f);
    g.Process(buf.data(), 1000);
    g.set_db(-20.0);
    std::vector<float> next(1000, 1.0f);
    g.Process(next.data(), 1000);
    ZC_CHECK(std::fabs(static_cast<double>(next[0]) - buf.back()) < 0.01);
    double worst = 0.0;
    for (size_t i = 1; i < next.size(); ++i) {
      worst = std::max(worst,
                       std::fabs(static_cast<double>(next[i]) - next[i - 1]));
    }
    ZC_CHECK(worst < 0.01);
  }
}
