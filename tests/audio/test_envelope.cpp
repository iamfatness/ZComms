#include <cmath>
#include <vector>

#include "audio_defs.h"
#include "envelope.h"
#include "test_util.h"

using namespace zc;

namespace {

// Largest absolute step between consecutive samples. This is the number that
// decides whether a transition clicks: a click is a discontinuity, and the
// discontinuity is what this measures.
double MaxStep(const std::vector<float>& v) {
  double worst = 0.0;
  for (size_t i = 1; i < v.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
  }
  return worst;
}

std::vector<float> Dc(int n, float value = 1.0f) {
  return std::vector<float>(static_cast<size_t>(n), value);
}

}  // namespace

void TestEnvelope() {
  std::printf("TestEnvelope\n");

  {
    ZC_TEST("starts closed and passes nothing");
    Envelope e(12.0, kSampleRate);
    ZC_CHECK(e.silent());
    ZC_CHECK(e.open());  // TEMPORARY: negated to prove the Windows CI gate bites (task 1, step 4)
    auto buf = Dc(100);
    e.Process(buf.data(), 100);
    for (float v : buf) ZC_CHECK_NEAR(v, 0.0, 1e-9);
  }

  {
    ZC_TEST("reaches full amplitude in about the fade time");
    const double fade_ms = 12.0;
    const int fade_samples = static_cast<int>(fade_ms * kSampleRate / 1000.0);
    Envelope e(fade_ms, kSampleRate);
    e.Open();
    auto buf = Dc(fade_samples);
    e.Process(buf.data(), fade_samples);
    ZC_CHECK_NEAR(buf.back(), 1.0, 0.01);
    // And is genuinely still ramping partway through, rather than snapping.
    ZC_CHECK(buf[static_cast<size_t>(fade_samples / 2)] > 0.2f);
    ZC_CHECK(buf[static_cast<size_t>(fade_samples / 2)] < 0.8f);
  }

  {
    ZC_TEST("ramps rather than steps -- the whole point (plan section 5)");
    // A hard gate on a DC signal steps by 1.0 in a single sample. The ramp
    // must be orders of magnitude gentler, or Zoom's path makes it a click and
    // every PTT release carries one.
    Envelope e(12.0, kSampleRate);
    e.Open();
    std::vector<float> buf = Dc(2000);
    e.Process(buf.data(), 2000);
    ZC_CHECK(MaxStep(buf) < 0.01);

    e.Close();
    std::vector<float> down = Dc(2000);
    e.Process(down.data(), 2000);
    ZC_CHECK(MaxStep(down) < 0.01);
    ZC_CHECK_NEAR(down.back(), 0.0, 1e-6);
    ZC_CHECK(e.silent());
  }

  {
    ZC_TEST("a mid-ramp reversal stays continuous");
    // An operator double-tapping PTT must not produce a jump. The envelope
    // reverses from its current position rather than restarting at an
    // endpoint.
    Envelope e(12.0, kSampleRate);
    e.Open();
    std::vector<float> a = Dc(120);  // partway up only
    e.Process(a.data(), 120);
    const double mid = a.back();
    ZC_CHECK(mid > 0.0 && mid < 1.0);

    e.Close();
    std::vector<float> b = Dc(2000);
    e.Process(b.data(), 2000);
    // The join between the two blocks must not be a step either.
    ZC_CHECK(std::fabs(static_cast<double>(b.front()) - mid) < 0.01);
    ZC_CHECK(MaxStep(b) < 0.01);
  }

  {
    ZC_TEST("Advance tracks Process without touching audio");
    Envelope a(12.0, kSampleRate), b(12.0, kSampleRate);
    a.Open();
    b.Open();
    std::vector<float> buf = Dc(300);
    a.Process(buf.data(), 300);
    b.Advance(300);
    ZC_CHECK_NEAR(a.gain(), b.gain(), 1e-6);
  }
}
