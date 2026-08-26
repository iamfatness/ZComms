// zcomms-engine -- drives the audio engine with no UI and no meeting.
//
// This is how the engine gets verified while Spike A is still blocked on a
// live measurement. --record writes exactly what would have been handed to
// Zoom's virtual mic, and --ptt-cycle scripts the press/release pattern, so
// the properties that matter can be checked by looking at a waveform rather
// than by listening to a meeting and forming an impression.
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "audio_defs.h"
#include "clock.h"
#include "devices.h"
#include "engine.h"
#include "wav_sink.h"

#pragma comment(lib, "winmm.lib")

using namespace zc;

namespace {

void PrintUsage() {
  std::printf(R"(zcomms-engine -- ZComms audio engine (plan sections 6.1-6.3)

Capture -> gain -> limiter -> PTT ramp -> 20 ms frames -> paced TX.
No UI, no meeting, no Zoom SDK required.

USAGE
  zcomms-engine [options]
  zcomms-engine --list-devices

OPTIONS
  --in <name>            Capture device substring. Default: system default.
  --out <name>           Monitor device substring. Default: system default.
  --no-monitor           Do not open an output device at all.
  --gain <dB>            Input gain. Default 0.
  --sidetone <dB>        Sidetone level. Default -12.
  --no-sidetone          Mute sidetone.
  --record <path.wav>    Write the TX stream to a WAV file.
  --seconds <n>          Run length. Default 10.
  --talk                 Hold PTT open for the whole run.
  --ptt-cycle <on,off>   Cycle PTT, milliseconds on then off. Verifies the
                         ramp shape without anyone holding a key.
  --list-devices         Show capture and playback devices, then exit.

NOTES
  Sidetone is tapped after the PTT envelope, so it is a confidence monitor of
  what is actually being sent. Silence while not transmitting is correct.

  Use a headset. Feeding raw PCM to Zoom's virtual mic bypasses Zoom's echo
  canceller entirely (plan section 2), so an open speaker echoes into the
  meeting for everyone.
)");
}

int ListDevices() {
  std::printf("Capture devices:\n");
  for (const auto& d : ListCaptureDevices()) {
    std::printf("  [%d] %s%s\n", d.index, d.name.c_str(),
                d.is_default ? "   (default)" : "");
  }
  std::printf("\nPlayback devices:\n");
  for (const auto& d : ListPlaybackDevices()) {
    std::printf("  [%d] %s%s\n", d.index, d.name.c_str(),
                d.is_default ? "   (default)" : "");
  }
  return 0;
}

std::string MeterBar(double peak) {
  // dBFS, floored at -60. A linear bar spends most of its width on levels an
  // operator never uses.
  const double db = peak > 1e-6 ? 20.0 * std::log10(peak) : -60.0;
  const double frac = std::max(0.0, std::min(1.0, (db + 60.0) / 60.0));
  const int filled = static_cast<int>(frac * 20.0);
  std::string bar(20, '.');
  for (int i = 0; i < filled && i < 20; ++i) bar[static_cast<size_t>(i)] = '#';
  return bar;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  EngineConfig cfg;
  std::string record_path;
  int seconds = 10;
  bool hold_talk = false;
  int cycle_on_ms = 0, cycle_off_ms = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::printf("ERROR: %s requires a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { PrintUsage(); return 0; }
    else if (a == "--list-devices") { return ListDevices(); }
    else if (a == "--in") cfg.capture_device = next("--in");
    else if (a == "--out") cfg.monitor_device = next("--out");
    else if (a == "--no-monitor") cfg.monitor_enabled = false;
    else if (a == "--gain") cfg.input_gain_db = std::atof(next("--gain"));
    else if (a == "--sidetone") cfg.sidetone_db = std::atof(next("--sidetone"));
    else if (a == "--no-sidetone") cfg.sidetone_enabled = false;
    else if (a == "--record") record_path = next("--record");
    else if (a == "--seconds") seconds = std::atoi(next("--seconds"));
    else if (a == "--talk") hold_talk = true;
    else if (a == "--ptt-cycle") {
      const std::string v = next("--ptt-cycle");
      const auto comma = v.find(',');
      if (comma == std::string::npos) {
        std::printf("ERROR: --ptt-cycle wants <on_ms>,<off_ms>\n");
        return 2;
      }
      cycle_on_ms = std::atoi(v.substr(0, comma).c_str());
      cycle_off_ms = std::atoi(v.substr(comma + 1).c_str());
    } else {
      std::printf("ERROR: unknown argument %s\n\n", a.c_str());
      PrintUsage();
      return 2;
    }
  }

  if (seconds <= 0) { std::printf("ERROR: --seconds must be positive\n"); return 2; }

  WavSink wav;
  FrameSink* sink = &wav;
  if (!record_path.empty()) {
    std::string err;
    if (!wav.Open(record_path, &err)) {
      std::printf("ERROR: %s\n", err.c_str());
      return 1;
    }
    std::printf("recording TX stream to %s\n", record_path.c_str());
  } else {
    // Still a WAV sink, just to a throwaway path -- the engine always has a
    // sink, and a null one would leave the pacer's send path untested in the
    // common case of "just let me hear myself".
    std::string err;
    if (!wav.Open("zcomms-engine-scratch.wav", &err)) {
      std::printf("ERROR: %s\n", err.c_str());
      return 1;
    }
  }

  // The pacer's 20 ms grid cannot be held on Windows' default ~15.6 ms timer
  // resolution.
  timeBeginPeriod(1);

  AudioEngine engine(cfg, sink);
  std::string err;
  if (!engine.Start(&err)) {
    std::printf("ERROR: %s\n", err.c_str());
    std::printf("Run --list-devices to see what is available.\n");
    timeEndPeriod(1);
    return 1;
  }

  std::printf("  in:  %s\n", engine.capture_device_name().c_str());
  if (cfg.monitor_enabled) {
    std::printf("  out: %s  (sidetone %.1f dB%s)\n",
                engine.monitor_device_name().c_str(), cfg.sidetone_db,
                cfg.sidetone_enabled ? "" : ", muted");
  }
  std::printf("  gain %.1f dB, PTT fade %.0f ms, limiter ceiling %.1f dBFS\n\n",
              cfg.input_gain_db, cfg.ptt_fade_ms, cfg.limiter_ceiling_dbfs);

  if (hold_talk) engine.SetTalk(true);

  const int64_t end_ns = NowNs() + static_cast<int64_t>(seconds) * 1'000'000'000LL;
  int64_t next_toggle_ns = NowNs();
  bool cycle_state = false;

  while (NowNs() < end_ns) {
    if (cycle_on_ms > 0 && NowNs() >= next_toggle_ns) {
      cycle_state = !cycle_state;
      engine.SetTalk(cycle_state);
      next_toggle_ns =
          NowNs() + static_cast<int64_t>(cycle_state ? cycle_on_ms : cycle_off_ms) *
                        1'000'000LL;
    }

    const EngineStats s = engine.stats();
    std::printf("\r  [%s] %-4s  frames %llu  underrun %llu  drops %llu  "
                "late %.2fms   ",
                MeterBar(s.capture_peak).c_str(),
                engine.talking() ? "TALK" : "",
                (unsigned long long)s.frames_to_ring,
                (unsigned long long)s.pacer.underruns,
                (unsigned long long)s.ring_drops, s.pacer.max_late_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  engine.SetTalk(false);
  // Let the release ramp finish before tearing down, or the recording ends
  // mid-fade and the file shows a step that the engine did not actually
  // produce.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  engine.Stop();

  const EngineStats s = engine.stats();
  wav.Close();
  timeEndPeriod(1);

  std::printf("\n\n===== engine =====\n");
  std::printf("  capture frames     %llu\n", (unsigned long long)s.capture_frames);
  std::printf("  monitor frames     %llu\n", (unsigned long long)s.monitor_frames);
  std::printf("  20ms frames to TX  %llu\n", (unsigned long long)s.frames_to_ring);
  std::printf("  ring drops         %llu\n", (unsigned long long)s.ring_drops);
  std::printf("  TX ticks           %llu\n", (unsigned long long)s.pacer.ticks);
  std::printf("  TX sends           %llu\n", (unsigned long long)s.pacer.sends);
  std::printf("  TX underruns       %llu\n", (unsigned long long)s.pacer.underruns);
  std::printf("  TX gated ticks     %llu\n", (unsigned long long)s.pacer.gated_ticks);
  std::printf("  tick lateness      %.2f ms mean, %.2f ms worst\n",
              s.pacer.mean_late_ms, s.pacer.max_late_ms);
  std::printf("  limiter engaged    %llu samples\n",
              (unsigned long long)s.limiter_engaged_samples);
  std::printf("  sidetone drop/starve %llu / %llu\n",
              (unsigned long long)s.sidetone_drops,
              (unsigned long long)s.sidetone_starved);
  if (!record_path.empty()) {
    std::printf("  wrote              %llu samples to %s\n",
                (unsigned long long)wav.samples_written(), record_path.c_str());
  }
  return 0;
}
