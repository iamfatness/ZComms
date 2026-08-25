// Spike A -- TX latency harness (plan §9).
//
// Four modes:
//   --list-devices  which output the second Zoom client should be playing to
//   --self-test     prove the instrument against a known delay, no SDK needed
//   --calibrate     measure the local render+loopback bias on this machine
//   (default)       join a meeting and measure the real thing
#include <windows.h>
// timeBeginPeriod lives here; WIN32_LEAN_AND_MEAN excludes mmsystem.h, which
// is where it would otherwise arrive from.
#include <timeapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "audio_defs.h"
#include "clock.h"
#include "config.h"
#include "generator.h"
#include "loopback.h"
#include "mic_source.h"
#include "probe.h"
#include "signal.h"
#include "stats.h"
#include "tx_pacer.h"
#include "zoom_client.h"

#pragma comment(lib, "winmm.lib")

namespace zc {
namespace {

constexpr double kKillThresholdMs = 250.0;

void PrintSummary(const char* title, const Summary& s, const ProbeStats& ps,
                  const PacerStats& tx) {
  std::printf("\n===== %s =====\n", title);
  if (s.n == 0) {
    std::printf("  NO SAMPLES RESOLVED -- see diagnostics below.\n");
  } else {
    std::printf("  samples          %zu\n", s.n);
    std::printf("  median (p50)     %8.1f ms\n", s.p50_ms);
    std::printf("  p95              %8.1f ms\n", s.p95_ms);
    std::printf("  min / max        %8.1f / %.1f ms\n", s.min_ms, s.max_ms);
    std::printf("  mean +/- sd      %8.1f +/- %.1f ms\n", s.mean_ms, s.stddev_ms);
    std::printf("  jitter p95-p50   %8.1f ms\n", s.jitter_p95_p50_ms);
    std::printf("  MAD              %8.1f ms\n", s.mad_ms);
  }
  std::printf("\n  -- diagnostics --\n");
  std::printf("  bursts emitted   %llu\n", (unsigned long long)ps.emitted);
  std::printf("  resolved         %llu\n", (unsigned long long)ps.resolved);
  std::printf("  no detection     %llu   (audio captured, no burst matched)\n",
              (unsigned long long)ps.no_detection);
  std::printf("  data gap         %llu   (capture missing or aged out)\n",
              (unsigned long long)ps.data_gap);
  // Only flag a quiet capture when nothing resolved. The correlator is
  // normalised, so it recovers bursts far below the level a human would call
  // audible -- a calibration run measured 13/13 at an RMS this check would
  // have called silent. Warning on level alone taught the reader to distrust
  // a good result.
  std::printf("  capture RMS      %.6f%s\n", ps.capture_rms,
              (ps.resolved == 0 && ps.capture_rms < 1e-5)
                  ? "   <-- nothing arriving; wrong device, or far end muted"
                  : "");
  std::printf("  capture rate     %.1f Hz (nominal %d)\n",
              ps.measured_capture_rate_hz, kSampleRate);
  std::printf("  TX ticks         %llu\n", (unsigned long long)tx.ticks);
  std::printf("  TX sends         %llu\n", (unsigned long long)tx.sends);
  std::printf("  TX underruns     %llu\n", (unsigned long long)tx.underruns);
  std::printf("  TX gated ticks   %llu   (outside the send window)\n",
              (unsigned long long)tx.gated_ticks);
  std::printf("  TX send errors   %llu\n", (unsigned long long)tx.send_errors);
  std::printf("  TX tick lateness %.2f ms mean, %.2f ms worst\n", tx.mean_late_ms,
              tx.max_late_ms);
}

void PrintVerdict(const Summary& s) {
  if (s.n == 0) return;
  std::printf("\n===== SPIKE A VERDICT (plan §9 kill criterion: ~%.0f ms) =====\n",
              kKillThresholdMs);
  if (s.p50_ms > kKillThresholdMs) {
    std::printf("  MEDIAN %.1f ms EXCEEDS the threshold.\n", s.p50_ms);
    std::printf("  The Zoom-transport intercom thesis does not hold for live\n"
                "  crew use. This is a result, not a failure: Phase 1 still\n"
                "  ships as producer-to-guest talkback, and Phase 3's native\n"
                "  fabric moves to the front of the queue.\n");
  } else if (s.p95_ms > kKillThresholdMs) {
    std::printf("  Median %.1f ms is inside the threshold but p95 %.1f ms is\n"
                "  not. The tail is what an operator hears as broken, so treat\n"
                "  this as marginal and look at the jitter figure before\n"
                "  committing Phase 1 to live crew use.\n", s.p50_ms, s.p95_ms);
  } else {
    std::printf("  Median %.1f ms and p95 %.1f ms are both inside the\n"
                "  threshold. The Zoom-transport thesis survives Spike A.\n",
                s.p50_ms, s.p95_ms);
  }
}

void WriteCsv(const std::string& path, const std::vector<LatencySample>& samples) {
  std::ofstream f(path);
  if (!f.is_open()) {
    std::printf("  (could not open %s for writing)\n", path.c_str());
    return;
  }
  f << "burst_id,latency_ms,corr_peak,psr\n";
  for (const auto& s : samples) {
    f << s.burst_id << "," << s.latency_ms << "," << s.peak << "," << s.psr << "\n";
  }
  std::printf("  wrote %zu samples to %s\n", samples.size(), path.c_str());
}

// ---------------------------------------------------------------------------
// The shared measurement run. Identical for Zoom and for calibration -- the
// only difference is which FrameSink the paced TX thread writes into, which is
// exactly what makes the calibration a like-for-like subtraction rather than a
// different experiment.
// ---------------------------------------------------------------------------
struct RunResult {
  Summary summary;
  ProbeStats probe_stats;
  PacerStats pacer_stats;
  std::vector<LatencySample> samples;
  bool capture_ok = false;
  std::string capture_device;
};

RunResult RunMeasurement(FrameSink* sink, const Config& cfg, int duration_s,
                         ZoomClient* zoom /*nullable*/) {
  RunResult result;
  SignalParams sp;

  FrameRing ring(50);
  SignalGenerator gen(&ring, sp);

  ProbeConfig pcfg;
  LatencyProbe probe(&gen.burst_up(), &gen.burst_down(), pcfg);

  LoopbackCapture capture;
  std::string cap_err;
  if (!capture.Start(cfg.loopback_device,
                     [&probe](const float* mono, int frames, int64_t host_ns) {
                       probe.OnCapture(mono, frames, host_ns);
                     },
                     &cap_err)) {
    std::printf("  ERROR: %s\n", cap_err.c_str());
    std::printf("  Run --list-devices to see what is available.\n");
    return result;
  }
  result.capture_ok = true;
  result.capture_device = capture.device_name();
  std::printf("  tapping output device: %s\n", capture.device_name().c_str());

  gen.Start();
  TxPacer pacer(&ring, sink,
                [&probe](int32_t id, bool up, int64_t ns) {
                  probe.OnEmission(id, up, ns);
                });
  if (!pacer.Start(3, 500)) {
    std::printf("  (priming timed out; starting anyway -- underruns will be "
                "counted)\n");
  }

  // Correlation runs on its own thread so it can never stall the Windows
  // message pump that the SDK's callbacks are delivered through.
  std::atomic<bool> resolving{true};
  std::thread resolver([&] {
    while (resolving.load()) {
      probe.Resolve();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });

  const int64_t end_ns = NowNs() + static_cast<int64_t>(duration_s) * 1'000'000'000LL;
  int64_t next_report_ns = NowNs() + 15'000'000'000LL;

  while (NowNs() < end_ns) {
    if (zoom != nullptr) {
      zoom->Pump(200);
      if (!zoom->in_meeting()) {
        std::printf("  meeting state changed to %s -- stopping early\n",
                    MeetingStatusName(zoom->status()));
        break;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (NowNs() >= next_report_ns) {
      next_report_ns = NowNs() + 15'000'000'000LL;
      const ProbeStats ps = probe.stats();
      const auto s = Summarise([&] {
        std::vector<double> v;
        for (const auto& x : probe.samples()) v.push_back(x.latency_ms);
        return v;
      }());
      const int64_t remaining_s = (end_ns - NowNs()) / 1'000'000'000LL;
      if (s.n > 0) {
        std::printf("  [%llds left] n=%zu  p50=%.1f ms  p95=%.1f ms  "
                    "(miss=%llu gap=%llu)\n",
                    (long long)remaining_s, s.n, s.p50_ms, s.p95_ms,
                    (unsigned long long)ps.no_detection,
                    (unsigned long long)ps.data_gap);
      } else {
        std::printf("  [%llds left] no samples yet -- emitted=%llu miss=%llu "
                    "gap=%llu rms=%.5f\n",
                    (long long)remaining_s, (unsigned long long)ps.emitted,
                    (unsigned long long)ps.no_detection,
                    (unsigned long long)ps.data_gap, ps.capture_rms);
      }
    }
  }

  pacer.Stop();
  gen.Stop();
  // Drain: bursts emitted in the final seconds still have their audio in
  // flight, and stopping the capture before it lands would throw away real
  // samples and bias the tail.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  resolving.store(false);
  resolver.join();
  probe.Resolve();
  capture.Stop();

  result.samples = probe.samples();
  result.probe_stats = probe.stats();
  result.pacer_stats = pacer.stats();
  std::vector<double> lat;
  for (const auto& s : result.samples) lat.push_back(s.latency_ms);
  result.summary = Summarise(lat);
  return result;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int DoListDevices() {
  const auto devices = ListPlaybackDevices();
  std::printf("Playback devices (the second Zoom client must be playing to the\n"
              "one you tap, and that device must not be muted at the OS level):\n\n");
  for (const auto& d : devices) {
    std::printf("  [%d] %s%s\n", d.index, d.name.c_str(),
                d.is_default ? "   (default)" : "");
  }
  if (devices.empty()) std::printf("  (none found)\n");
  return 0;
}

// Proves the measurement chain on this machine against a delay we chose.
// The live run has no ground truth, so this is the only place the instrument
// can be checked rather than trusted.
int DoSelfTest() {
  std::printf("Self-test: recovering known delays through the full "
              "signal/correlation/timebase chain.\n\n");
  SignalParams sp;
  const std::vector<float> up = MakeBurst(sp, true);
  const std::vector<float> down = MakeBurst(sp, false);

  bool all_ok = true;
  for (double truth_ms : {45.0, 150.0, 275.0, 600.0}) {
    ProbeConfig cfg;
    LatencyProbe probe(&up, &down, cfg);

    const double rate = 48000.0 * (1.0 + 200e-6);  // a clock that is not nominal
    const int64_t t0 = NowNs();
    const int bursts = 10;

    std::vector<int64_t> emits;
    for (int k = 0; k < bursts; ++k) {
      emits.push_back(t0 + static_cast<int64_t>(k * sp.period_ms * 1e6));
    }

    const double total_ms = bursts * sp.period_ms + truth_ms + 500.0;
    std::vector<float> cap(static_cast<size_t>(total_ms / 1000.0 * rate));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (auto& v : cap) v = noise(rng) * 0.02f;
    for (int k = 0; k < bursts; ++k) {
      const double idx =
          (static_cast<double>(emits[k] + static_cast<int64_t>(truth_ms * 1e6) - t0) /
           1e9) * rate;
      const std::vector<float>& b = (k % 2 == 0) ? up : down;
      const size_t base = static_cast<size_t>(std::llround(idx));
      for (size_t i = 0; i < b.size(); ++i) {
        if (base + i < cap.size()) cap[base + i] += b[i] * 0.6f;
      }
    }

    std::uniform_real_distribution<double> jitter(-3e6, 3e6);
    size_t pos = 0;
    int next = 0, since = 0;
    while (pos < cap.size()) {
      const size_t n = std::min<size_t>(480, cap.size() - pos);
      const int64_t clean = t0 + static_cast<int64_t>((pos + n) / rate * 1e9);
      while (next < bursts && emits[next] <= clean) {
        probe.OnEmission(next, (next % 2) == 0, emits[next]);
        ++next;
      }
      probe.OnCapture(cap.data() + pos, static_cast<int>(n),
                      clean + static_cast<int64_t>(jitter(rng)));
      pos += n;
      if (++since >= 20) { since = 0; probe.Resolve(); }
    }
    probe.Resolve();

    std::vector<double> lat;
    for (const auto& s : probe.samples()) lat.push_back(s.latency_ms);
    const Summary s = Summarise(lat);
    const double err = s.n ? std::fabs(s.p50_ms - truth_ms) : 1e9;
    const bool ok = s.n >= 8 && err <= 2.0;
    all_ok = all_ok && ok;
    std::printf("  truth %6.1f ms -> recovered %6.1f ms  (n=%zu, error %.2f ms)  %s\n",
                truth_ms, s.n ? s.p50_ms : 0.0, s.n, s.n ? err : 0.0,
                ok ? "OK" : "FAIL");
  }

  std::printf("\n%s\n", all_ok
      ? "Self-test PASSED -- the instrument recovers a known delay to within 2 ms."
      : "Self-test FAILED -- do not trust a live number from this build.");
  return all_ok ? 0 : 1;
}

int DoCalibrate(const Config& cfg) {
  std::printf("Calibration: measuring the local render + loopback bias.\n\n"
              "  This plays the probe signal out of an output device and taps\n"
              "  that same device. The result is everything the measurement\n"
              "  path costs *without* Zoom in it: our own render buffering plus\n"
              "  the capture-side plumbing.\n\n"
              "  It is an over-estimate of the part that is not Zoom's -- the\n"
              "  far client's own render buffering is genuinely Zoom latency and\n"
              "  is included in both numbers. So it brackets rather than\n"
              "  corrects: true Zoom latency lies in (measured - bias, measured].\n\n");

  PlaybackSink sink;
  std::string err;
  if (!sink.Start(cfg.loopback_device, &err)) {
    std::printf("  ERROR: %s\n", err.c_str());
    return 1;
  }
  std::printf("  playing to: %s\n", sink.device_name().c_str());

  const int duration = cfg.duration_s > 60 ? 60 : cfg.duration_s;
  const RunResult r = RunMeasurement(&sink, cfg, duration, nullptr);
  sink.Stop();

  if (!r.capture_ok) return 1;
  PrintSummary("LOCAL BIAS (no Zoom in the path)", r.summary, r.probe_stats,
               r.pacer_stats);
  if (r.summary.n > 0) {
    std::printf("\n  Subtract %.1f ms from a measured Zoom figure for the lower\n"
                "  bound of the bracket.\n", r.summary.p50_ms);
  }
  if (!cfg.csv_path.empty()) WriteCsv(cfg.csv_path, r.samples);
  return 0;
}

int DoCheckAuth(const Config& cfg) {
  std::printf("Auth check: initialising and authenticating the Meeting SDK.\n\n");
  if (cfg.public_app_key.empty() && cfg.sdk_key.empty()) {
    std::printf("ERROR: no credentials. Set public_app_key (or sdk_key +\n"
                "       sdk_secret) in local.env -- see local.env.example.\n");
    return 1;
  }
  ZoomClient zoom;
  std::string err;
  if (!zoom.Init(&err)) {
    std::printf("ERROR: %s\n", err.c_str());
    return 1;
  }
  if (!zoom.Authenticate(cfg.public_app_key, cfg.sdk_key, cfg.sdk_secret, 30000,
                         &err)) {
    std::printf("\nAUTH FAILED: %s\n", err.c_str());
    zoom.Cleanup();
    return 1;
  }
  std::printf("\nAUTH OK -- credentials are good. The SDK is ready to join a "
              "meeting.\n");
  zoom.Cleanup();
  return 0;
}

int DoMeasure(const Config& cfg) {
  std::printf("Spike A -- measuring one-way latency into meeting %llu\n\n",
              (unsigned long long)cfg.meeting_number);
  std::printf("  BEFORE THIS RUNS, on this machine:\n"
              "    1. A second Zoom client is joined to the same meeting.\n"
              "    2. Its microphone is MUTED. If it is not, its mic picks up\n"
              "       the loopback and feeds it back into the meeting, and the\n"
              "       measurement is of a loop rather than of a path.\n"
              "    3. Its speaker is the device being tapped, and that device\n"
              "       is not muted at the OS level.\n\n");

  ZoomClient zoom;
  std::string err;
  if (!zoom.Init(&err)) {
    std::printf("ERROR: %s\n", err.c_str());
    return 1;
  }
  if (!zoom.Authenticate(cfg.public_app_key, cfg.sdk_key, cfg.sdk_secret, 30000,
                         &err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Cleanup();
    return 1;
  }
  std::printf("[sdk] authenticated\n");

  std::printf("[sdk] joining as \"%s\" -- admit it if a waiting room is on\n",
              cfg.display_name.c_str());
  if (!zoom.Join(cfg.meeting_number, cfg.meeting_password, cfg.display_name,
                 cfg.join_timeout_s * 1000, &err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Cleanup();
    return 1;
  }
  std::printf("[sdk] in meeting\n");

  ZoomMicSource mic;
  if (!zoom.InstallVirtualMic(&mic, &err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Leave();
    zoom.Cleanup();
    return 1;
  }
  if (!zoom.JoinVoip(&err)) {
    std::printf("WARNING: %s (continuing -- audio may already be joined)\n",
                err.c_str());
  }

  // The virtual mic's callbacks arrive on the message pump, so give them a
  // chance before deciding anything is wrong.
  std::printf("[sdk] waiting for the virtual mic to initialise...\n");
  for (int i = 0; i < 100 && !mic.CanSend(); ++i) zoom.Pump(100);
  if (!mic.CanSend()) {
    std::printf("WARNING: the send window never opened (onMicStartSend not\n"
                "         seen). Continuing -- every tick will be counted as\n"
                "         gated, which is the diagnostic you want.\n");
  }

  const RunResult r = RunMeasurement(&mic, cfg, cfg.duration_s, &zoom);

  zoom.Leave();
  zoom.Cleanup();

  if (!r.capture_ok) return 1;
  PrintSummary("ONE-WAY LATENCY: ZComms virtual mic -> second Zoom client",
               r.summary, r.probe_stats, r.pacer_stats);
  std::printf("  mic send failures %llu (last SDKError %d)\n",
              (unsigned long long)mic.send_failures(), mic.last_error());
  PrintVerdict(r.summary);
  if (!cfg.csv_path.empty()) WriteCsv(cfg.csv_path, r.samples);
  return 0;
}

}  // namespace
}  // namespace zc

namespace zc {
namespace {

// Leaves the process without running DLL detach handlers.
//
// This is not tidiness-avoidance; it works around a fault in the Zoom SDK's
// own teardown. Returning from main normally reaches:
//
//   ExitProcess -> LdrShutdownProcess -> sdk.dll DLL_PROCESS_DETACH
//     -> execute_onexit_table -> ZoomTask!GetZMTManager
//     -> std::_Throw_Cpp_error -> unhandled -> fastfail 0xC0000409
//
// Zoom's atexit handler does threading work under the loader lock and throws
// std::system_error when it fails. It is inside sdk.dll, it fires whether or
// not InitSDK was ever called, and there is nothing callable on our side that
// prevents it -- so it crashed even `--help`, purely because sdk.dll is
// statically imported and therefore always loaded.
//
// TerminateProcess skips LdrShutdownProcess entirely. Everything that owns
// real state -- CleanUPSDK, the audio devices, the CSV file -- has already
// been shut down through its own destructor by the time we get here, so the
// only thing being skipped is the handler that crashes. stdio is flushed
// explicitly first, because TerminateProcess will not do it and a lost report
// would be a much worse bug than an untidy exit.
[[noreturn]] void HardExit(int code) {
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
  // Not reached; present so the compiler can see the noreturn contract holds.
  for (;;) {}
}

}  // namespace
}  // namespace zc

int main(int argc, char** argv) {
  // Unbuffered stdout. The default when stdout is a pipe or a file is 4 KB
  // block buffering, which for a run that prints a progress line every 15
  // seconds means the log stays empty for minutes and then arrives all at
  // once. During the first live runs that made a harness sitting in a waiting
  // room indistinguishable from one that had joined and was measuring.
  //
  // _IONBF, not _IOLBF: on Win32 the CRT documents _IOLBF as behaving the
  // same as full buffering, so line buffering silently does nothing here.
  // Progress output is a handful of lines a minute -- unbuffered costs
  // nothing and is the only setting that actually works on this platform.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  zc::Config cfg;
  std::string error;
  if (!zc::ParseConfig(argc, argv, &cfg, &error)) {
    std::printf("ERROR: %s\n\n", error.c_str());
    zc::PrintUsage();
    zc::HardExit(2);
  }
  if (cfg.mode == zc::Mode::kHelp) {
    zc::PrintUsage();
    zc::HardExit(0);
  }

  // Windows' default timer resolution is ~15.6 ms. A 20 ms TX cadence cannot
  // be held on that, and the pacer's short spin only closes the last stretch.
  timeBeginPeriod(1);
  int rc = 0;
  switch (cfg.mode) {
    case zc::Mode::kListDevices: rc = zc::DoListDevices(); break;
    case zc::Mode::kCheckAuth:   rc = zc::DoCheckAuth(cfg); break;
    case zc::Mode::kSelfTest:    rc = zc::DoSelfTest(); break;
    case zc::Mode::kCalibrate:   rc = zc::DoCalibrate(cfg); break;
    case zc::Mode::kMeasure:     rc = zc::DoMeasure(cfg); break;
    case zc::Mode::kHelp:        break;  // handled above
  }
  timeEndPeriod(1);
  zc::HardExit(rc);
}
