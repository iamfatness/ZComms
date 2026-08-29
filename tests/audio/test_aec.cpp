// The echo canceller, proven against a synthetic acoustic path.
//
// A live echo test needs a speaker, a microphone and a room; this needs
// none of them, because the acoustic path is just a delay and an
// attenuation, and both ends of the canceller are fed programmatically:
// far-end audio goes in via FeedPlayback, the same audio -- delayed,
// attenuated -- comes back as the "microphone" signal, and the output must
// be quiet. That is the whole ship-blocking scenario from plan §2 in
// miniature: monitor audio leaking into the mic.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "aec.h"
#include "audio_defs.h"
#include "test_util.h"

using namespace zc;

namespace {

double EnergyDb(const std::vector<float>& v, size_t from) {
  double sq = 0.0;
  size_t n = 0;
  for (size_t i = from; i < v.size(); ++i, ++n) {
    sq += static_cast<double>(v[i]) * v[i];
  }
  if (n == 0 || sq <= 0.0) return -120.0;
  return 10.0 * std::log10(sq / static_cast<double>(n));
}

// Far-end "speech": band-limited noise, more speech-like for the adaptive
// filter than a sine (a single tone under-excites the filter and flatters
// nobody).
std::vector<float> FarEnd(int samples, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> dist(0.0f, 0.15f);
  std::vector<float> v(static_cast<size_t>(samples));
  float lp = 0.0f;
  for (auto& s : v) {
    lp = 0.9f * lp + 0.1f * dist(rng);  // crude low-pass, speech-ish
    s = lp * 4.0f;
  }
  return v;
}

}  // namespace

void TestAec() {
  std::printf("TestAec\n");

  {
    ZC_TEST("cancels a delayed, attenuated echo of its own reference");
    // 6 seconds: the first stretch is convergence, judged only on the tail.
    const int seconds = 6;
    const int total = kSampleRate * seconds;
    const int delay = kSampleRate * 40 / 1000;  // 40 ms speaker-to-mic
    const float gain = 0.5f;                    // -6 dB acoustic loss

    EchoCanceller aec(kSampleRate, kFrameSamples, 200);
    const std::vector<float> far = FarEnd(total, 42);

    std::vector<float> out;
    out.reserve(static_cast<size_t>(total));
    std::vector<float> mic(static_cast<size_t>(kFrameSamples));

    for (int off = 0; off + kFrameSamples <= total; off += kFrameSamples) {
      // Playback first, exactly as the engine does (monitor callback runs
      // ahead of the sound reaching the mic).
      aec.FeedPlayback(far.data() + off, kFrameSamples);
      // The "microphone": the far end through the acoustic path, nothing
      // else. A perfect canceller outputs silence.
      for (int i = 0; i < kFrameSamples; ++i) {
        const int src = off + i - delay;
        mic[static_cast<size_t>(i)] =
            src >= 0 ? far[static_cast<size_t>(src)] * gain : 0.0f;
      }
      aec.ProcessCapture(mic.data());
      out.insert(out.end(), mic.begin(), mic.end());
    }

    // Echo-in level over the last 2 s vs what the canceller left of it.
    std::vector<float> echo_in;
    echo_in.reserve(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
      const int src = i - delay;
      echo_in.push_back(src >= 0 ? far[static_cast<size_t>(src)] * gain : 0.0f);
    }
    const size_t tail = static_cast<size_t>(total - 2 * kSampleRate);
    const double in_db = EnergyDb(echo_in, tail);
    const double out_db = EnergyDb(out, tail);
    const double erle = in_db - out_db;
    std::printf("    echo in %.1f dB -> residual %.1f dB (ERLE %.1f dB)\n",
                in_db, out_db, erle);
    // 15 dB is the bar for "no longer a conversation-wrecker"; a converged
    // linear canceller on a clean synthetic path should sail past it.
    ZC_CHECK(erle >= 15.0);
  }

  {
    ZC_TEST("passes near-end speech through when there is no echo");
    // Double-check the cure is not worse than the disease: with a silent far
    // end, the mic signal must come through essentially untouched.
    EchoCanceller aec(kSampleRate, kFrameSamples, 200);
    const std::vector<float> speech = FarEnd(kSampleRate * 2, 7);
    std::vector<float> silence(static_cast<size_t>(kFrameSamples), 0.0f);
    std::vector<float> out;
    std::vector<float> mic(static_cast<size_t>(kFrameSamples));
    for (int off = 0; off + kFrameSamples <= kSampleRate * 2;
         off += kFrameSamples) {
      aec.FeedPlayback(silence.data(), kFrameSamples);
      std::copy(speech.begin() + off, speech.begin() + off + kFrameSamples,
                mic.begin());
      aec.ProcessCapture(mic.data());
      out.insert(out.end(), mic.begin(), mic.end());
    }
    const size_t tail = static_cast<size_t>(kSampleRate);
    const double in_db = EnergyDb(speech, tail);
    const double out_db = EnergyDb(out, tail);
    std::printf("    speech in %.1f dB -> out %.1f dB (delta %.1f dB)\n",
                in_db, out_db, in_db - out_db);
    // Within 3 dB: transparent enough that an operator would never notice.
    ZC_CHECK(std::fabs(in_db - out_db) <= 3.0);
  }

  {
    ZC_TEST("bypass leaves audio untouched");
    EchoCanceller aec(kSampleRate, kFrameSamples, 200);
    aec.SetEnabled(false);
    std::vector<float> mic(static_cast<size_t>(kFrameSamples));
    for (int i = 0; i < kFrameSamples; ++i) {
      mic[static_cast<size_t>(i)] = 0.25f * std::sin(0.01f * i);
    }
    const std::vector<float> before = mic;
    aec.ProcessCapture(mic.data());
    bool same = true;
    for (int i = 0; i < kFrameSamples; ++i) {
      if (mic[static_cast<size_t>(i)] != before[static_cast<size_t>(i)]) {
        same = false;
        break;
      }
    }
    ZC_CHECK(same);
  }
}
