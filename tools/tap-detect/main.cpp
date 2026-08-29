// zcomms-tap -- taps every playback endpoint and reports where the ZComms
// test signal is arriving, by matched filter.
//
// The far end of the end-to-end test. zcomms --test-signal transmits Spike
// A's chirp probe (alternating up/down bursts, one per second) into the
// talkback channel; whichever endpoint the listening client renders to will
// contain bursts the correlator can find.
//
// Matched filtering, not tone energy, on purpose: the first field runs of the
// Goertzel version were defeated by ordinary life -- a livestream on the same
// bus, a client joined with original-sound streaming raw room noise. Both
// raise a tone detector's off-band floor until the verdict drowns. A chirp's
// correlation against broadband audio stays near zero while a real burst
// stands ~1000x proud, and the spike proved the detector against known delays
// to 0.1 ms. Detection here = at least kMinBursts bursts clearing the same
// peak/PSR gates the spike shipped with.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_defs.h"
#include "correlator.h"
#include "devices.h"
#include "loopback.h"
#include "signal.h"

using namespace zc;

namespace {

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

  // The same waveforms zcomms transmits. Regenerated rather than shared over
  // a wire so the tool stays standalone; SignalParams defaults must match the
  // generator's (they are the shared defaults in signal.h).
  SignalParams sp;
  const std::vector<float> up = MakeBurst(sp, true);
  const std::vector<float> down = MakeBurst(sp, false);
  DetectorConfig dcfg;  // spike defaults: peak >= 0.15, PSR >= 3.0

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

  // One burst per second is transmitted; require enough to rule out a fluke,
  // scaled to how long we listened.
  const int min_bursts = seconds >= 6 ? 3 : 2;

  std::printf("\n%-34s %8s %8s %8s  %s\n", "endpoint", "bursts", "peak",
              "psr", "verdict");
  bool any = false;
  std::string winner;
  int winner_count = 0;

  for (auto& t : taps) {
    std::vector<float> x;
    {
      std::lock_guard<std::mutex> lock(t->m);
      x.swap(t->samples);
    }
    // Slide a 1.5 s window in 1 s steps; each window holds at most one burst
    // of each polarity, so counting found-windows counts bursts.
    const size_t win = static_cast<size_t>(kSampleRate) * 3 / 2;
    const size_t step = static_cast<size_t>(kSampleRate);
    int found = 0;
    double best_peak = 0.0, best_psr = 0.0;
    for (size_t off = 0; off + win <= x.size(); off += step) {
      std::vector<float> slice(x.begin() + static_cast<std::ptrdiff_t>(off),
                               x.begin() + static_cast<std::ptrdiff_t>(off + win));
      const Detection du = FindBurst(slice, up, dcfg);
      const Detection dd = FindBurst(slice, down, dcfg);
      const Detection& best = du.peak >= dd.peak ? du : dd;
      if (best.peak > best_peak) {
        best_peak = best.peak;
        best_psr = best.psr;
      }
      if (du.found || dd.found) ++found;
    }
    const bool hit = found >= min_bursts;
    std::printf("%-34s %8d %8.3f %8.1f  %s\n", t->name.c_str(), found,
                best_peak, best_psr, hit ? "PROBE DETECTED" : "-");
    if (hit) {
      any = true;
      if (found > winner_count) {
        winner_count = found;
        winner = t->name;
      }
    }
  }

  if (any) {
    std::printf("\nPASS: probe heard on \"%s\" (%d bursts)\n", winner.c_str(),
                winner_count);
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  std::printf("\nFAIL: probe not heard on any endpoint\n");
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), 1);
}
