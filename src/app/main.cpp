// zcomms -- the talkback panel. Phase 1, first vertical slice.
//
// Joins the client's meeting as a co-host participant, puts every other
// participant on one private talkback channel, and gives the operator a key:
// hold SPACE, talk to the panelists; release, silence. The audience and the
// meeting recording hear nothing. That is the product's core loop (plan §1),
// and everything else in Phase 1 hangs off it.
//
// What is deliberately NOT here yet: multiple channels (the engine and the
// talkback source are built for it; the UI is not), AEC (plan §2 --
// ship-blocking for release; this build states the headset requirement
// loudly instead), sign-in, packaging.
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#include <conio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio_defs.h"
#include "clock.h"
#include "control_server.h"
#include "devices.h"
#include "engine.h"
#include "frame_ring.h"
#include "roster.h"
#include "talkback_source.h"
#include "tx_pacer.h"
#include "ui_html.h"
#include "zoom_client.h"

#pragma comment(lib, "winmm.lib")

namespace zc {
namespace {

// sdk.dll fastfails the process in its own DLL_PROCESS_DETACH (see the Spike A
// harness, where the full stack is documented). Every exit goes through here.
[[noreturn]] void HardExit(int code) {
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
  for (;;) {}
}

struct AppConfig {
  std::string public_app_key;
  uint64_t meeting_number = 0;
  std::string meeting_password;
  std::string display_name = "ZComms";
  std::string mic_device;      // capture, substring match
  std::string monitor_device;  // sidetone output, substring match
  double gain_db = 0.0;
  bool sidetone = true;
  // Start with talk latched on. For unattended verification, and for an
  // operator who wants an open channel rather than PTT.
  bool start_latched = false;
  // Replace the microphone with an internally generated 700/1000 Hz beep
  // pattern. Exists so an automated end-to-end run can prove
  // engine -> channel -> listener with a signal a detector can find; the mic
  // capture path itself is verified separately on hardware.
  bool test_signal = false;
  int run_seconds = 0;  // 0 = run until Q
  // The browser panel / control API (plan §6.4). 0 disables.
  uint16_t ui_port = 7350;
  bool open_browser = false;
};

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
  }
  return out;
}

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool ParseMeetingArg(const std::string& input, uint64_t* number,
                     std::string* password) {
  const auto pwd_pos = input.find("pwd=");
  if (pwd_pos != std::string::npos && password->empty()) {
    const size_t start = pwd_pos + 4;
    size_t end = input.find_first_of("&#", start);
    if (end == std::string::npos) end = input.size();
    *password = input.substr(start, end - start);
  }
  std::string best, cur;
  const auto flush = [&]() {
    if (cur.size() > best.size()) best = cur;
    cur.clear();
  };
  for (char c : input) {
    if (c >= '0' && c <= '9') {
      cur.push_back(c);
    } else {
      if (c == ' ' && !cur.empty()) continue;
      flush();
    }
  }
  flush();
  if (best.size() < 9 || best.size() > 12) return false;
  *number = std::strtoull(best.c_str(), nullptr, 10);
  return *number != 0;
}

void LoadLocalEnv(AppConfig* cfg, std::string* meeting_arg) {
  // Same gitignored file the spike uses, searched from cwd upward one level,
  // so running from the repo root or from src/app both work.
  for (const char* path : {"local.env", "spikes/a-tx-latency/local.env",
                           "../local.env"}) {
    std::ifstream f(path);
    if (!f.is_open()) continue;
    std::string line;
    while (std::getline(f, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == '#') continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = Trim(line.substr(0, eq));
      const std::string val = Trim(line.substr(eq + 1));
      if (key == "public_app_key" && cfg->public_app_key.empty())
        cfg->public_app_key = val;
      else if (key == "meeting_number" && meeting_arg->empty())
        *meeting_arg = val;
      else if (key == "meeting_password" && cfg->meeting_password.empty())
        cfg->meeting_password = val;
      else if (key == "display_name")
        cfg->display_name = val;
      else if (key == "mic_device" && cfg->mic_device.empty())
        cfg->mic_device = val;
      else if (key == "monitor_device" && cfg->monitor_device.empty())
        cfg->monitor_device = val;
    }
    return;
  }
}

void PrintUsage() {
  std::printf(R"(zcomms -- talkback panel for a Zoom meeting

Joins the meeting, puts everyone else on a private talkback channel, and
talks to them while SPACE is held. Nobody outside the channel hears it.

USAGE
  zcomms --meeting <url|id> [options]
  zcomms --list-devices

OPTIONS
  --meeting <url|id>   Meeting to join (or meeting_number in local.env).
  --passcode <pw>      Meeting passcode if not in the URL.
  --name <s>           Display name in the participant list. Default ZComms.
  --in <s>             Microphone device substring. Default: system default.
  --out <s>            Sidetone output device substring.
  --no-sidetone        Do not open an output device.
  --gain <dB>          Input gain. Default 0.

KEYS (while running)
  SPACE (hold)   talk to the channel
  L              latch talk on/off
  M              sidetone on/off
  + / -          input gain
  Q              leave and quit

USE A HEADSET. Raw PCM into Zoom bypasses its echo cancellation (plan
section 2): on open speakers your monitor feeds back into the channel.
)");
}

std::string MeterBar(double peak) {
  const double db = peak > 1e-6 ? 20.0 * std::log10(peak) : -60.0;
  const double frac = db < -60.0 ? 0.0 : (db > 0.0 ? 1.0 : (db + 60.0) / 60.0);
  const int filled = static_cast<int>(frac * 16.0);
  std::string bar(16, '.');
  for (int i = 0; i < filled && i < 16; ++i) bar[static_cast<size_t>(i)] = '#';
  return bar;
}

int Run(int argc, char** argv) {
  AppConfig cfg;
  std::string meeting_arg;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::printf("ERROR: %s requires a value\n", what);
        HardExit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { PrintUsage(); return 0; }
    else if (a == "--list-devices") {
      std::printf("Capture devices:\n");
      for (const auto& d : ListCaptureDevices())
        std::printf("  %s%s\n", d.name.c_str(), d.is_default ? "  (default)" : "");
      std::printf("Playback devices:\n");
      for (const auto& d : ListPlaybackDevices())
        std::printf("  %s%s\n", d.name.c_str(), d.is_default ? "  (default)" : "");
      return 0;
    }
    else if (a == "--meeting") meeting_arg = next("--meeting");
    else if (a == "--passcode") cfg.meeting_password = next("--passcode");
    else if (a == "--name") cfg.display_name = next("--name");
    else if (a == "--in") cfg.mic_device = next("--in");
    else if (a == "--out") cfg.monitor_device = next("--out");
    else if (a == "--no-sidetone") cfg.sidetone = false;
    else if (a == "--gain") cfg.gain_db = std::atof(next("--gain"));
    else if (a == "--latch") cfg.start_latched = true;
    else if (a == "--test-signal") cfg.test_signal = true;
    else if (a == "--seconds") cfg.run_seconds = std::atoi(next("--seconds"));
    else if (a == "--no-ui") cfg.ui_port = 0;
    else if (a == "--ui-port") cfg.ui_port = static_cast<uint16_t>(std::atoi(next("--ui-port")));
    else if (a == "--open") cfg.open_browser = true;
    else {
      std::printf("ERROR: unknown argument %s\n\n", a.c_str());
      PrintUsage();
      return 2;
    }
  }

  LoadLocalEnv(&cfg, &meeting_arg);

  if (meeting_arg.empty() ||
      !ParseMeetingArg(meeting_arg, &cfg.meeting_number, &cfg.meeting_password)) {
    std::printf("ERROR: no meeting. Pass --meeting <url|id> or set "
                "meeting_number in local.env.\n\n");
    PrintUsage();
    return 2;
  }
  if (cfg.public_app_key.empty()) {
    std::printf("ERROR: no public_app_key found in local.env.\n");
    return 2;
  }

  std::printf("zcomms -- talkback panel\n\n");
  std::printf("  USE A HEADSET: no echo cancellation on this path yet.\n\n");

  // --- Zoom bring-up --------------------------------------------------------
  ZoomClient zoom;
  std::string err;
  if (!zoom.Init(&err)) {
    std::printf("ERROR: %s\n", err.c_str());
    std::printf("  (if this is error 14: another app's Zoom engine is "
                "running -- close it first)\n");
    return 1;
  }
  if (!zoom.Authenticate(cfg.public_app_key, "", "", 30000, &err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Cleanup();
    return 1;
  }
  std::printf("[zoom] joining meeting %llu as \"%s\" -- admit it if a waiting "
              "room is on\n",
              (unsigned long long)cfg.meeting_number, cfg.display_name.c_str());
  if (!zoom.Join(cfg.meeting_number, cfg.meeting_password, cfg.display_name,
                 600000, &err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Cleanup();
    return 1;
  }
  std::printf("[zoom] in the meeting\n");
  if (!zoom.JoinVoip(&err)) {
    std::printf("WARNING: %s\n", err.c_str());
  }

  Roster roster;
  roster.Attach(zoom.GetParticipantsController());

  // --- Channel bring-up (co-host retry, as proven in the spike) -------------
  ZoomTalkbackSource channel(zoom.GetTalkbackController());
  if (!channel.meeting_supports_talkback()) {
    std::printf("ERROR: this meeting does not support talkback.\n");
    zoom.Leave();
    zoom.Cleanup();
    return 1;
  }
  bool created = false;
  for (int attempt = 0; attempt < 120 && !created; ++attempt) {
    if (channel.CreateChannel(&err)) {
      for (int i = 0; i < 100 && !channel.channel_ready(); ++i) zoom.Pump(100);
      created = channel.channel_ready();
    }
    if (!created) {
      std::printf(">>> promote \"%s\" to CO-HOST in the participant list "
                  "(retrying, %d)\n",
                  cfg.display_name.c_str(), attempt + 1);
      zoom.Pump(5000);
    }
  }
  if (!created) {
    std::printf("ERROR: could not create a talkback channel.\n");
    zoom.Leave();
    zoom.Cleanup();
    return 1;
  }
  channel.SetBackgroundVolume(0.2f);  // duck, don't erase, the meeting
  std::printf("[zoom] channel up\n");

  // --- Audio path -----------------------------------------------------------
  // Normal mode: the engine, mic through gain/limiter/PTT into the channel.
  // Test mode: an internally generated beep pattern through the same ring and
  // pacer, so the paced TX path is exercised identically -- only the source
  // differs.
  std::unique_ptr<AudioEngine> engine;
  std::unique_ptr<FrameRing> test_ring;
  std::unique_ptr<TxPacer> test_pacer;
  std::atomic<bool> test_running{false};
  std::thread test_gen;

  if (cfg.test_signal) {
    std::printf("[audio] TEST SIGNAL mode: 700/1000 Hz beep pattern\n");
    test_ring = std::make_unique<FrameRing>(50);
    test_pacer = std::make_unique<TxPacer>(test_ring.get(), &channel, nullptr);
    test_running.store(true);
    test_gen = std::thread([&test_ring, &test_running]() {
      // 300 ms beep, 200 ms gap, alternating 700 and 1000 Hz, -12 dBFS, with
      // 10 ms raised-cosine edges so the pattern is click-free like real PTT
      // audio would be.
      const double kAmp = 0.25;
      const int beep = 48 * 300, gap = 48 * 200, ramp = 48 * 10;
      uint64_t seq = 0;
      int pos = 0;
      bool high = false;
      const int64_t period_ns = 20'000'000;
      const int64_t start = NowNs();
      int64_t produced = 0;
      while (test_running.load()) {
        TxFrame f;
        f.seq = seq++;
        for (int i = 0; i < kFrameSamples; ++i) {
          double v = 0.0;
          if (pos < beep) {
            const double f_hz = high ? 1000.0 : 700.0;
            double w = 1.0;
            if (pos < ramp) w = 0.5 * (1.0 - std::cos(3.14159265 * pos / ramp));
            else if (pos >= beep - ramp)
              w = 0.5 * (1.0 - std::cos(3.14159265 * (beep - 1 - pos) / ramp));
            v = kAmp * w *
                std::sin(2.0 * 3.14159265 * f_hz * (pos / 48000.0));
          }
          f.pcm[i] = static_cast<int16_t>(v * 32767.0);
          if (++pos >= beep + gap) {
            pos = 0;
            high = !high;
          }
        }
        test_ring->Push(f);
        ++produced;
        const int64_t target = start + produced * period_ns;
        const int64_t now = NowNs();
        if (now < target)
          std::this_thread::sleep_for(std::chrono::nanoseconds(target - now));
      }
    });
    test_pacer->Start(3, 500);
  } else {
    EngineConfig ecfg;
    ecfg.capture_device = cfg.mic_device;
    ecfg.monitor_device = cfg.monitor_device;
    ecfg.monitor_enabled = cfg.sidetone;
    ecfg.input_gain_db = cfg.gain_db;
    engine = std::make_unique<AudioEngine>(ecfg, &channel);
    if (!engine->Start(&err)) {
      std::printf("ERROR: %s\n", err.c_str());
      zoom.Leave();
      zoom.Cleanup();
      return 1;
    }
    std::printf("[audio] mic: %s\n", engine->capture_device_name().c_str());
    if (cfg.sidetone) {
      std::printf("[audio] sidetone: %s\n", engine->monitor_device_name().c_str());
    }
  }
  std::printf("\nHold SPACE to talk. L latch, M sidetone, +/- gain, Q quit.\n\n");

  // --- Main loop ------------------------------------------------------------
  // One thread does everything except audio: pumps the SDK, reads keys, heals
  // channel membership, redraws status. The audio path never waits on it.
  // --- Control surface (the browser panel; plan §6.4) -----------------------
  // Actions land on server threads and are applied here on the main loop, so
  // everything below stays single-threaded. talk is level-state (the key is
  // held or it is not); the rest are edge requests.
  std::atomic<bool> ui_talk{false};
  std::atomic<int> latch_req{-1}, side_req{-1};
  std::atomic<int> gain_req{0};
  std::atomic<bool> gain_pending{false};
  std::atomic<bool> quit_req{false};

  std::unique_ptr<ControlServer> ui;
  if (cfg.ui_port != 0) {
    ui = std::make_unique<ControlServer>(
        kPanelHtml,
        [&](const std::string& verb, const std::string& arg) {
          if (verb == "talk") ui_talk.store(arg == "on");
          else if (verb == "latch") latch_req.store(arg == "on" ? 1 : 0);
          else if (verb == "sidetone") side_req.store(arg == "on" ? 1 : 0);
          else if (verb == "gain") {
            gain_req.store(std::atoi(arg.c_str()));
            gain_pending.store(true);
          } else if (verb == "quit") quit_req.store(true);
        });
    std::string ui_err;
    if (ui->Start(cfg.ui_port, &ui_err)) {
      std::printf("[ui] panel at http://127.0.0.1:%u\n", cfg.ui_port);
      if (cfg.open_browser) {
        const std::string url = "http://127.0.0.1:" + std::to_string(cfg.ui_port);
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
    } else {
      std::printf("WARNING: %s -- running without the panel\n", ui_err.c_str());
      ui.reset();
    }
  }

  // Ops log: the last few operational lines, mirrored to the panel's ticker.
  std::vector<std::string> ops_log;
  const auto log_op = [&](const std::string& line) {
    std::printf("\n[zcomms] %s\n", line.c_str());
    ops_log.push_back(line);
    if (ops_log.size() > 5) ops_log.erase(ops_log.begin());
  };
  log_op("station up -- channel " + channel.channel_id());

  std::set<unsigned int> invited;
  bool latched = cfg.start_latched;
  bool quit = false;
  bool talking = false;
  double gain_db = cfg.gain_db;
  bool sidetone_on = cfg.sidetone;
  const int64_t end_ns =
      cfg.run_seconds > 0
          ? NowNs() + static_cast<int64_t>(cfg.run_seconds) * 1'000'000'000LL
          : 0;

  while (!quit && zoom.in_meeting()) {
    if (end_ns != 0 && NowNs() >= end_ns) break;
    zoom.Pump(30);

    // Host duties + membership healing, on a coarse cadence. Invite anyone
    // not yet invited who supports talkback (the web client does not --
    // inviting it fails with INVALID_PARAMETER); Zoom drops leavers on its
    // own, and a rejoin arrives as a brand-new user id (plan §5). Retried on
    // a timer rather than only on roster changes, so a transient failure
    // heals instead of sticking.
    static int64_t next_house_ns = 0;
    roster.ConsumeDirty();
    if (NowNs() >= next_house_ns) {
      next_house_ns = NowNs() + 2'000'000'000LL;
      zoom.AdmitAllWaiting();  // no-op unless we are host

      std::vector<unsigned int> fresh;
      for (const RosterMember& m : roster.others()) {
        if (invited.count(m.user_id) != 0) continue;
        if (!m.supports_talkback) {
          static std::set<unsigned int> warned;
          if (warned.insert(m.user_id).second) {
            log_op(m.name + " cannot receive talkback (web client) -- skipped");
          }
          continue;
        }
        fresh.push_back(m.user_id);
      }
      if (!fresh.empty()) {
        if (channel.InviteUsers(fresh, &err)) {
          for (unsigned int uid : fresh) invited.insert(uid);
          log_op("invited " + std::to_string(fresh.size()) +
                 " participant(s) to the channel");
        } else {
          log_op("invite failed: " + err);
        }
      }
    }

    // Publish the panel state. Built every tick and cheap; the server samples
    // it at its own cadence, so a slow tab costs nothing here.
    if (ui) {
      std::string status = MeetingStatusName(zoom.status());
      for (char& c : status) {
        if (c == '_') c = ' ';
      }
      double peak = 0.0;
      uint64_t sends = 0, unders = 0;
      if (engine) {
        const EngineStats es = engine->stats();
        peak = es.capture_peak;
        sends = es.pacer.sends;
        unders = es.pacer.underruns;
      } else if (test_pacer) {
        const PacerStats p = test_pacer->stats();
        sends = p.sends;
        unders = p.underruns;
        peak = talking || true ? 0.25 : 0.0;  // test tone nominal level
      }
      std::string j = "{";
      j += "\"meeting\":\"" + std::to_string(cfg.meeting_number) + "\",";
      j += "\"status\":\"" + status + "\",";
      j += std::string("\"channel_ready\":") +
           (channel.channel_ready() ? "true," : "false,");
      j += "\"listeners\":" + std::to_string(channel.users_joined()) + ",";
      j += std::string("\"talking\":") + (talking ? "true," : "false,");
      j += std::string("\"latched\":") + (latched ? "true," : "false,");
      j += std::string("\"sidetone\":") + (sidetone_on ? "true," : "false,");
      j += "\"gain\":" + std::to_string(static_cast<int>(gain_db)) + ",";
      char pk[32];
      std::snprintf(pk, sizeof(pk), "%.4f", peak);
      j += std::string("\"peak\":") + pk + ",";
      j += "\"sends\":" + std::to_string(sends) + ",";
      j += "\"underruns\":" + std::to_string(unders) + ",";
      j += "\"fails\":" + std::to_string(channel.send_failures()) + ",";
      j += "\"roster\":[";
      bool first = true;
      for (const RosterMember& m : roster.others()) {
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"" + JsonEscape(m.name) + "\",";
        j += std::string("\"tb\":") + (m.supports_talkback ? "true," : "false,");
        j += std::string("\"invited\":") +
             (invited.count(m.user_id) ? "true}" : "false}");
      }
      j += "],\"log\":[";
      first = true;
      for (const std::string& l : ops_log) {
        if (!first) j += ",";
        first = false;
        j += "\"" + JsonEscape(l) + "\"";
      }
      j += "]}";
      ui->PublishState(j);
    }

    // Apply control-surface edges on the main loop.
    if (quit_req.load()) quit = true;
    {
      const int lr = latch_req.exchange(-1);
      if (lr >= 0) latched = lr == 1;
      const int sr = side_req.exchange(-1);
      if (sr >= 0) {
        sidetone_on = sr == 1;
        if (engine) engine->SetSidetoneEnabled(sidetone_on);
      }
      if (gain_pending.exchange(false)) {
        gain_db = static_cast<double>(gain_req.load());
        if (engine) engine->SetInputGainDb(gain_db);
      }
    }

    // PTT: physical SPACE held, OR the panel's key held, OR latched. The
    // envelope in the engine makes rapid toggling safe by design.
    const bool space_down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    talking = latched || space_down || ui_talk.load();
    if (engine) engine->SetTalk(talking);

    while (_kbhit()) {
      const int c = _getch();
      switch (c) {
        case 'q': case 'Q': quit = true; break;
        case 'l': case 'L':
          latched = !latched;
          break;
        case 'm': case 'M':
          sidetone_on = !sidetone_on;
          if (engine) engine->SetSidetoneEnabled(sidetone_on);
          break;
        case '+': case '=':
          gain_db += 1.0;
          if (engine) engine->SetInputGainDb(gain_db);
          break;
        case '-': case '_':
          gain_db -= 1.0;
          if (engine) engine->SetInputGainDb(gain_db);
          break;
        default: break;
      }
    }

    if (engine) {
      const EngineStats s = engine->stats();
      std::printf("\r  [%s] %-6s  ch:%d listener(s)  gain %+.0f dB  "
                  "underrun %llu   ",
                  MeterBar(s.capture_peak).c_str(),
                  (latched ? "LATCH" : (space_down ? "TALK" : "")),
                  channel.users_joined(), gain_db,
                  (unsigned long long)s.pacer.underruns);
    } else if (test_pacer) {
      const PacerStats p = test_pacer->stats();
      std::printf("\r  [test-signal] ch:%d listener(s)  sends %llu  "
                  "underrun %llu  fail %llu   ",
                  channel.users_joined(), (unsigned long long)p.sends,
                  (unsigned long long)p.underruns,
                  (unsigned long long)channel.send_failures());
    }
  }

  // --- Teardown -------------------------------------------------------------
  std::printf("\n[zoom] leaving...\n");
  if (engine) {
    engine->SetTalk(false);
    Sleep(100);  // let the release ramp finish before the engine stops
    engine->Stop();
  }
  if (test_pacer) test_pacer->Stop();
  if (test_gen.joinable()) {
    test_running.store(false);
    test_gen.join();
  }
  zoom.Leave();
  zoom.Cleanup();
  return 0;
}

}  // namespace
}  // namespace zc

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  timeBeginPeriod(1);
  const int rc = zc::Run(argc, argv);
  timeEndPeriod(1);
  zc::HardExit(rc);
}
