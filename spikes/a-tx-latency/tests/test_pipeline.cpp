// End-to-end test of the measurement chain against a known ground truth.
//
// This is the test that licenses any confidence in the reported number. The
// live run has no ground truth by construction -- that is the whole reason the
// spike exists -- so the only way to know the instrument is honest is to feed
// it a delay we chose and check it hands the same delay back.
//
// The simulation deliberately includes the three things that could bias a real
// run: a capture clock that is not exactly 48 kHz, callback delivery jitter of
// several milliseconds, and additive noise on the signal.
#include <cmath>
#include <random>
#include <vector>

#include "audio_defs.h"
#include "clock.h"
#include "probe.h"
#include "signal.h"
#include "stats.h"
#include "test_util.h"

using namespace zc;

namespace {

struct SimResult {
  Summary summary;
  ProbeStats stats;
};

SimResult RunSimulation(double true_latency_ms, double ppm, double anchor_jitter_ms,
                        float noise_amp, int burst_count, unsigned seed) {
  SignalParams sp;
  const std::vector<float> up = MakeBurst(sp, true);
  const std::vector<float> down = MakeBurst(sp, false);

  ProbeConfig cfg;
  // Narrower than the live default purely to keep the test fast; the live
  // harness searches far wider so a bad result is still measurable.
  cfg.max_latency_ms = 300.0;
  LatencyProbe probe(&up, &down, cfg);

  const double true_rate = 48000.0 * (1.0 + ppm * 1e-6);
  const int64_t t0 = NowNs();
  const double period_ms = sp.period_ms;

  // Emission schedule, on the one clock.
  std::vector<int64_t> emit_ns(static_cast<size_t>(burst_count));
  for (int k = 0; k < burst_count; ++k) {
    emit_ns[static_cast<size_t>(k)] =
        t0 + static_cast<int64_t>(k * period_ms * 1e6);
  }

  const double total_ms = burst_count * period_ms + true_latency_ms + 500.0;
  const size_t total_samples =
      static_cast<size_t>(total_ms / 1000.0 * true_rate);

  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
  std::vector<float> capture(total_samples);
  for (auto& v : capture) v = noise(rng) * noise_amp;

  // Plant each burst where a real one would land: at the capture index whose
  // host time is emission + true latency.
  for (int k = 0; k < burst_count; ++k) {
    const int64_t arrive_ns =
        emit_ns[static_cast<size_t>(k)] +
        static_cast<int64_t>(true_latency_ms * 1e6);
    const double idx = (static_cast<double>(arrive_ns - t0) / 1e9) * true_rate;
    const std::vector<float>& burst = (k % 2 == 0) ? up : down;
    const size_t base = static_cast<size_t>(std::llround(idx));
    for (size_t i = 0; i < burst.size(); ++i) {
      if (base + i < capture.size()) capture[base + i] += burst[i] * 0.6f;
    }
  }

  // Feed the probe in 10 ms chunks, with jittered anchor times, resolving as
  // we go -- the same order of operations as the live run, which matters
  // because the capture history is bounded and a batch-then-resolve pass would
  // not exercise the ageing-out path.
  std::uniform_real_distribution<double> jitter(-anchor_jitter_ms * 1e6,
                                                anchor_jitter_ms * 1e6);
  const size_t chunk = 480;
  size_t pos = 0;
  int next_emit = 0;
  int chunks_since_resolve = 0;

  while (pos < capture.size()) {
    const size_t n = std::min(chunk, capture.size() - pos);
    const uint64_t cumulative = pos + n;
    const int64_t clean_ns =
        t0 + static_cast<int64_t>(cumulative / true_rate * 1e9);
    const int64_t anchor_ns = clean_ns + static_cast<int64_t>(jitter(rng));

    while (next_emit < burst_count &&
           emit_ns[static_cast<size_t>(next_emit)] <= clean_ns) {
      probe.OnEmission(next_emit, (next_emit % 2) == 0,
                       emit_ns[static_cast<size_t>(next_emit)]);
      ++next_emit;
    }

    probe.OnCapture(capture.data() + pos, static_cast<int>(n), anchor_ns);
    pos += n;

    if (++chunks_since_resolve >= 20) {
      chunks_since_resolve = 0;
      probe.Resolve();
    }
  }
  probe.Resolve();

  std::vector<double> latencies;
  for (const auto& s : probe.samples()) latencies.push_back(s.latency_ms);

  SimResult r;
  r.summary = Summarise(latencies);
  r.stats = probe.stats();
  return r;
}

}  // namespace

void TestPipeline() {
  std::printf("TestPipeline\n");

  {
    ZC_TEST("recovers a known 180 ms delay under jitter, drift and noise");
    const SimResult r = RunSimulation(180.0, 250.0, 3.0, 0.02f, 12, 11);
    ZC_CHECK(r.stats.resolved >= 10);
    ZC_CHECK(r.stats.no_detection == 0);
    ZC_CHECK_NEAR(r.summary.p50_ms, 180.0, 2.0);
    ZC_CHECK_NEAR(r.summary.p95_ms, 180.0, 4.0);
  }

  {
    ZC_TEST("recovers a delay above the kill threshold");
    // If the product's answer is "too slow", the instrument still has to
    // report the actual figure. A harness whose search window or matching
    // quietly degraded past 250 ms would turn a real result into a
    // measurement failure and invite exactly the wrong conclusion.
    const SimResult r = RunSimulation(275.0, 100.0, 2.0, 0.02f, 10, 12);
    ZC_CHECK(r.stats.resolved >= 8);
    ZC_CHECK_NEAR(r.summary.p50_ms, 275.0, 2.0);
  }

  {
    ZC_TEST("recovers a short delay");
    const SimResult r = RunSimulation(45.0, -150.0, 2.0, 0.02f, 10, 13);
    ZC_CHECK(r.stats.resolved >= 8);
    ZC_CHECK_NEAR(r.summary.p50_ms, 45.0, 2.0);
  }

  {
    ZC_TEST("reports nothing rather than something wrong when audio is silent");
    // The failure mode to avoid above all others: a run where the far end is
    // muted or the wrong device is tapped must come back as zero samples, not
    // as a plausible-looking distribution.
    SignalParams sp;
    const std::vector<float> up = MakeBurst(sp, true);
    const std::vector<float> down = MakeBurst(sp, false);
    ProbeConfig cfg;
    cfg.max_latency_ms = 300.0;
    LatencyProbe probe(&up, &down, cfg);

    const int64_t t0 = NowNs();
    const double rate = 48000.0;
    std::mt19937 rng(21);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> silence(480);

    for (int k = 0; k < 6; ++k) {
      probe.OnEmission(k, (k % 2) == 0, t0 + static_cast<int64_t>(k * 2000.0 * 1e6));
    }
    for (int i = 1; i <= 900; ++i) {
      for (auto& v : silence) v = noise(rng) * 0.0005f;  // dither-level hiss
      const uint64_t cumulative = static_cast<uint64_t>(i) * 480;
      probe.OnCapture(silence.data(), 480,
                      t0 + static_cast<int64_t>(cumulative / rate * 1e9));
    }
    probe.Resolve();
    ZC_CHECK(probe.samples().empty());
    ZC_CHECK(probe.stats().resolved == 0);
  }
}
