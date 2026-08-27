// zcomms-tap -- taps every playback endpoint and reports where the ZComms
// test signal (700/1000 Hz beep pattern) is arriving.
//
// The end-to-end verifier's far end. zcomms --test-signal transmits into the
// talkback channel; a Zoom client in the meeting renders what it hears to
// some endpoint (empirically the default-communications one, which is why
// this taps all of them rather than trusting any name); this tool listens on
// every endpoint at once, runs Goertzel detectors at exactly the two beep
// frequencies plus an off-frequency reference, and prints per-device energy.
//
// Detection is a ratio, not a threshold on absolute level: beep-band energy
// must dominate both the off-band reference and the device's total energy,
// so neither music on another bus nor a quiet render defeats it.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_defs.h"
#include "devices.h"
#include "loopback.h"

using namespace zc;

namespace {

// Goertzel power of one frequency over a block of samples.
double Goertzel(const std::vector<float>& x, double f_hz) {
  const double w = 2.0 * 3.14159265358979 * f_hz / kSampleRate;
  const double coeff = 2.0 * std::cos(w);
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  for (float v : x) {
    s0 = v + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

struct Tap {
  std::string name;
  std::unique_ptr<LoopbackCapture> capture;
  std::mutex m;
  std::vector<float> samples;
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int seconds = 8;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--seconds" && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
    }
  }

  std::vector<std::unique_ptr<Tap>> taps;
  for (const auto& d : ListPlaybackDevices()) {
    auto t = std::make_unique<Tap>();
    t->name = d.name;
    t->capture = std::make_unique<LoopbackCapture>();
    Tap* raw = t.get();
    std::string err;
    if (!t->capture->Start(d.name,
                           [raw](const float* mono, int n, int64_t) {
                             std::lock_guard<std::mutex> lock(raw->m);
                             raw->samples.insert(raw->samples.end(), mono,
                                                 mono + n);
                           },
                           &err)) {
      std::printf("(skip %s: %s)\n", d.name.c_str(), err.c_str());
      continue;
    }
    taps.push_back(std::move(t));
  }
  if (taps.empty()) {
    std::printf("ERROR: nothing to tap\n");
    return 2;
  }

  std::printf("listening on %zu endpoint(s) for %d s...\n", taps.size(),
              seconds);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  for (auto& t : taps) t->capture->Stop();

  std::printf("\n%-34s %10s %10s %10s  %s\n", "endpoint", "beep", "offband",
              "rms", "verdict");
  bool any = false;
  std::string winner;
  double winner_score = 0.0;

  for (auto& t : taps) {
    std::vector<float> x;
    {
      std::lock_guard<std::mutex> lock(t->m);
      x.swap(t->samples);
    }
    // Analyse in 100 ms blocks and take the strongest beep block: the signal
    // is 300 ms on / 200 ms off, so whole-capture averaging would dilute it.
    const size_t block = kSampleRate / 10;
    double best_beep = 0.0, off_at_best = 1e-12, rms_total = 0.0;
    size_t blocks = 0;
    for (size_t off = 0; off + block <= x.size(); off += block, ++blocks) {
      std::vector<float> b(x.begin() + static_cast<std::ptrdiff_t>(off),
                           x.begin() + static_cast<std::ptrdiff_t>(off + block));
      const double e700 = Goertzel(b, 700.0);
      const double e1000 = Goertzel(b, 1000.0);
      // Off-frequencies bracketing the beeps: real beep energy stands far
      // above them; broadband audio (music, speech) does not.
      const double eoff = Goertzel(b, 550.0) + Goertzel(b, 850.0) +
                          Goertzel(b, 1300.0);
      const double beep = e700 + e1000;
      if (beep > best_beep) {
        best_beep = beep;
        off_at_best = eoff + 1e-12;
      }
      double sq = 0.0;
      for (float v : b) sq += static_cast<double>(v) * v;
      rms_total += sq;
    }
    const double rms =
        blocks > 0 ? std::sqrt(rms_total / (static_cast<double>(blocks) * block))
                   : 0.0;
    const double ratio = best_beep / off_at_best;
    // Detected: tonal dominance of at least 20x over the off-band at the best
    // block, and enough absolute level that it cannot be dither.
    const bool hit = ratio > 20.0 && rms > 1e-4;
    std::printf("%-34s %10.2e %10.2e %10.5f  %s\n", t->name.c_str(), best_beep,
                off_at_best, rms, hit ? "BEEPS DETECTED" : "-");
    if (hit) {
      any = true;
      if (ratio > winner_score) {
        winner_score = ratio;
        winner = t->name;
      }
    }
  }

  if (any) {
    std::printf("\nPASS: test signal heard on \"%s\"\n", winner.c_str());
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  std::printf("\nFAIL: test signal not heard on any endpoint\n");
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), 1);
}
