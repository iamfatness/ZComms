#include <cmath>
#include <random>

#include "audio_defs.h"
#include "clock.h"
#include "test_util.h"
#include "timebase.h"

using namespace zc;

void TestTimebase() {
  std::printf("TestTimebase\n");

  {
    ZC_TEST("recovers a device rate that is not nominal");
    // Real capture crystals are not 48000.000 Hz and are not disciplined to
    // QPC. If the harness assumed nominal, index-to-time would be wrong by a
    // slope error rather than by noise.
    const double true_rate = 48000.0 * (1.0 + 250e-6);
    CaptureTimebase tb(200);
    const int64_t t0 = 1'000'000'000;
    for (int i = 1; i <= 300; ++i) {
      const uint64_t idx = static_cast<uint64_t>(i) * 480;
      const int64_t ns = t0 + static_cast<int64_t>(idx / true_rate * 1e9);
      tb.AddAnchor(idx, ns);
    }
    double hz = 0.0;
    ZC_CHECK(tb.MeasuredRateHz(&hz));
    ZC_CHECK_NEAR(hz, true_rate, 1.0);
  }

  {
    ZC_TEST("averages callback jitter instead of inheriting it");
    // Each anchor carries several ms of delivery jitter. A mapping built from
    // the newest anchor alone would carry all of it into every measurement;
    // the fit should reduce it by roughly the square root of the window.
    const double rate = 48000.0;
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> jitter(-3e6, 3e6);  // +/-3 ms
    CaptureTimebase tb(200);
    const int64_t t0 = 5'000'000'000;
    for (int i = 1; i <= 400; ++i) {
      const uint64_t idx = static_cast<uint64_t>(i) * 480;
      const int64_t clean = t0 + static_cast<int64_t>(idx / rate * 1e9);
      tb.AddAnchor(idx, clean + static_cast<int64_t>(jitter(rng)));
    }
    const uint64_t probe_idx = 400ull * 480;
    const int64_t expected = t0 + static_cast<int64_t>(probe_idx / rate * 1e9);
    int64_t got = 0;
    ZC_CHECK(tb.HostNsForFrame(static_cast<double>(probe_idx), &got));
    ZC_CHECK_NEAR(NsToMs(got - expected), 0.0, 1.0);
  }

  {
    ZC_TEST("refuses to answer before it has enough anchors");
    // Returning a confident wrong time early would poison the first samples of
    // every run, which is exactly where a reader looks first.
    CaptureTimebase tb(200);
    int64_t ns = 0;
    ZC_CHECK(!tb.Ready());
    ZC_CHECK(!tb.HostNsForFrame(1000.0, &ns));
    for (int i = 1; i <= 8; ++i) tb.AddAnchor(static_cast<uint64_t>(i) * 480, i * 10'000'000);
    ZC_CHECK(tb.Ready());
    ZC_CHECK(tb.HostNsForFrame(1000.0, &ns));
  }

  {
    ZC_TEST("round-trips index to time and back");
    CaptureTimebase tb(200);
    const int64_t t0 = 2'000'000'000;
    for (int i = 1; i <= 100; ++i) {
      const uint64_t idx = static_cast<uint64_t>(i) * 480;
      tb.AddAnchor(idx, t0 + static_cast<int64_t>(idx / 48000.0 * 1e9));
    }
    int64_t ns = 0;
    ZC_CHECK(tb.HostNsForFrame(24000.0, &ns));
    double back = 0.0;
    ZC_CHECK(tb.FrameForHostNs(ns, &back));
    ZC_CHECK_NEAR(back, 24000.0, 1.0);
  }
}
