// Pins the "actual audio" detector both ducks are gated on: threshold,
// instant attack, hang across gaps, sample-count time (never wall clock).
#include <vector>

#include "audio_defs.h"
#include "signal_gate.h"
#include "test_util.h"

namespace {

std::vector<int16_t> Frame(int16_t value) {
  return std::vector<int16_t>(zc::kFrameSamples, value);
}

}  // namespace

void TestSignalGate() {
  ZC_TEST("signal_gate: silence never opens it");
  {
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto quiet = Frame(0);
    for (int i = 0; i < 100; ++i) g.Update(quiet.data(), zc::kFrameSamples);
    ZC_CHECK(!g.active());
  }

  ZC_TEST("signal_gate: instant attack on a hot frame");
  {
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto hot = Frame(3000);
    ZC_CHECK(g.Update(hot.data(), zc::kFrameSamples));
    ZC_CHECK(g.active());
  }

  ZC_TEST("signal_gate: below threshold stays closed");
  {
    // -50 dBFS ~ 103; a floor of 50 must not trip it.
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto floor_noise = Frame(50);
    ZC_CHECK(!g.Update(floor_noise.data(), zc::kFrameSamples));
  }

  ZC_TEST("signal_gate: hang bridges a speech gap, then releases");
  {
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto hot = Frame(3000);
    const auto quiet = Frame(0);
    g.Update(hot.data(), zc::kFrameSamples);
    // 800 ms hang = 40 x 20 ms frames of silence before release.
    for (int i = 0; i < 39; ++i) g.Update(quiet.data(), zc::kFrameSamples);
    ZC_CHECK(g.active());  // still inside the hang
    g.Update(quiet.data(), zc::kFrameSamples);
    ZC_CHECK(!g.active());  // 40th silent frame releases it
  }

  ZC_TEST("signal_gate: re-trigger mid-hang restarts the hang");
  {
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto hot = Frame(3000);
    const auto quiet = Frame(0);
    g.Update(hot.data(), zc::kFrameSamples);
    for (int i = 0; i < 30; ++i) g.Update(quiet.data(), zc::kFrameSamples);
    g.Update(hot.data(), zc::kFrameSamples);  // word two
    for (int i = 0; i < 39; ++i) g.Update(quiet.data(), zc::kFrameSamples);
    ZC_CHECK(g.active());
  }

  ZC_TEST("signal_gate: null frame counts as silence time");
  {
    zc::SignalGate g(-50.0, 800, zc::kSampleRate);
    const auto hot = Frame(3000);
    g.Update(hot.data(), zc::kFrameSamples);
    for (int i = 0; i < 40; ++i) g.Update(nullptr, zc::kFrameSamples);
    ZC_CHECK(!g.active());
  }
}
