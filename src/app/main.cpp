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
#include <timeapi.h>
#include <conio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio_defs.h"
#include "clock.h"
#include "devices.h"
#include "engine.h"
#include "roster.h"
#include "talkback_source.h"
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
};

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

  // --- Audio engine ---------------------------------------------------------
  EngineConfig ecfg;
  ecfg.capture_device = cfg.mic_device;
  ecfg.monitor_device = cfg.monitor_device;
  ecfg.monitor_enabled = cfg.sidetone;
  ecfg.input_gain_db = cfg.gain_db;
  AudioEngine engine(ecfg, &channel);
  if (!engine.Start(&err)) {
    std::printf("ERROR: %s\n", err.c_str());
    zoom.Leave();
    zoom.Cleanup();
    return 1;
  }
  std::printf("[audio] mic: %s\n", engine.capture_device_name().c_str());
  if (cfg.sidetone) {
    std::printf("[audio] sidetone: %s\n", engine.monitor_device_name().c_str());
  }
  std::printf("\nHold SPACE to talk. L latch, M sidetone, +/- gain, Q quit.\n\n");

  // --- Main loop ------------------------------------------------------------
  // One thread does everything except audio: pumps the SDK, reads keys, heals
  // channel membership, redraws status. The audio path never waits on it.
  std::set<unsigned int> invited;
  bool latched = false;
  bool quit = false;
  double gain_db = cfg.gain_db;

  while (!quit && zoom.in_meeting()) {
    zoom.Pump(30);

    // Membership healing. Invite anyone not yet invited; Zoom drops leavers
    // on its own, and a rejoin arrives as a brand-new user id (plan §5).
    if (roster.ConsumeDirty()) {
      std::vector<unsigned int> fresh;
      for (const RosterMember& m : roster.others()) {
        if (invited.insert(m.user_id).second) fresh.push_back(m.user_id);
      }
      if (!fresh.empty()) {
        if (channel.InviteUsers(fresh, &err)) {
          std::printf("\n[zoom] invited %zu participant(s) to the channel\n",
                      fresh.size());
        } else {
          std::printf("\nWARNING: %s\n", err.c_str());
        }
      }
    }

    // PTT: SPACE held = talking. GetAsyncKeyState reads the physical key, so
    // hold-to-talk works without keyup events, which the console cannot give
    // us. The envelope in the engine makes rapid toggling safe by design.
    const bool space_down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    engine.SetTalk(latched || space_down);

    while (_kbhit()) {
      const int c = _getch();
      switch (c) {
        case 'q': case 'Q': quit = true; break;
        case 'l': case 'L':
          latched = !latched;
          break;
        case 'm': case 'M': {
          static bool st = cfg.sidetone;
          st = !st;
          engine.SetSidetoneEnabled(st);
          break;
        }
        case '+': case '=':
          gain_db += 1.0;
          engine.SetInputGainDb(gain_db);
          break;
        case '-': case '_':
          gain_db -= 1.0;
          engine.SetInputGainDb(gain_db);
          break;
        default: break;
      }
    }

    const EngineStats s = engine.stats();
    std::printf("\r  [%s] %-6s  ch:%d listener(s)  gain %+.0f dB  "
                "underrun %llu   ",
                MeterBar(s.capture_peak).c_str(),
                (latched ? "LATCH" : (space_down ? "TALK" : "")),
                channel.users_joined(), gain_db,
                (unsigned long long)s.pacer.underruns);
  }

  // --- Teardown -------------------------------------------------------------
  std::printf("\n[zoom] leaving...\n");
  engine.SetTalk(false);
  Sleep(100);  // let the release ramp finish before the engine stops
  engine.Stop();
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
