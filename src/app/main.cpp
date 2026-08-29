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
#include <tlhelp32.h>
#include <conio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app_identity.h"
#include "audio_defs.h"
#include "clock.h"
#include "control_server.h"
#ifdef ZCOMMS_HAVE_WEBVIEW2
#include "shell_window.h"
#endif
#include "devices.h"
#include "engine.h"
#include "frame_ring.h"
#include "generator.h"
#include "roster.h"
#include "signal.h"
#include "talkback_channels.h"
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
  bool aec = true;
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
  // The panel opens itself by default: a packaged app's face is the panel,
  // and a double-clicked exe that silently listens on a port looks broken.
  bool open_browser = true;
  // Talkback channels to create (1..16). Participants land on CH 1 by
  // default; the panel moves them.
  int channels = 1;
};

// The seam between the paced TX thread and the channel bank. "Nothing keyed"
// is a normal quiet state, not a send failure.
class ChannelBankSink : public FrameSink {
 public:
  explicit ChannelBankSink(TalkbackChannels* bank) : bank_(bank) {}
  bool CanSend() override { return bank_->channels_ready() > 0; }
  bool Send(const int16_t* pcm, int samples) override {
    if (bank_->key_mask() == 0) return true;
    return bank_->SendToKeyed(pcm, samples) > 0;
  }

 private:
  TalkbackChannels* bank_;
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
  // Search order: dev files near the repo first, then the installed
  // location -- %APPDATA%\ZComms\config.env -- which is where a packaged
  // install keeps overrides. No file at all is a fully working state: the
  // app identity is baked in, and the meeting comes from the command line or
  // the interactive prompt.
  std::vector<std::string> paths = {"local.env",
                                    "spikes/a-tx-latency/local.env",
                                    "../local.env"};
  char* appdata = nullptr;
  size_t len = 0;
  if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
    paths.push_back(std::string(appdata) + "\\ZComms\\config.env");
    free(appdata);
  }
  for (const std::string& path : paths) {
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
  --channels <n>       Talkback channels to create, 1..16. Default 1. New
                       participants land on CH 1; the panel moves them.
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

// Names any other process on the machine known to embed the Zoom SDK. When
// InitSDK fails with OTHER_SDK_INSTANCE_RUNNING (14), "close the other app"
// is only actionable if we say WHICH app.
std::string SdkConflictHint() {
  const char* known[] = {"ZoomObsEngine.exe", "zcomms.exe", "ZoomISO.exe"};
  std::set<std::string> seen;  // the engine can run as several processes
  std::string found;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ProcessID == self) continue;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name),
                            nullptr, nullptr);
        for (const char* k : known) {
          if (_stricmp(name, k) == 0 && seen.insert(k).second) {
            if (!found.empty()) found += ", ";
            found += name;
          }
        }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }
  if (found.empty()) return "another app that embeds the Zoom SDK is running";
  return "close " + found + " (it holds the Zoom SDK) and try again";
}

// Opens the panel as a standalone app window rather than a browser tab:
// Edge/Chrome --app mode gives a chromeless window with its own taskbar
// entry, which is the difference between "a tab I lose" and "the app".
// Falls back to the default browser when neither is found.
void OpenAppWindow(const std::string& url) {
  const char* candidates[] = {
      "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
      "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
      "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
      "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  };
  for (const char* exe : candidates) {
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) continue;
    const std::string args = "--app=" + url + " --window-size=1000,640";
    if (reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", exe,
                                                args.c_str(), nullptr,
                                                SW_SHOWNORMAL)) > 32) {
      return;
    }
  }
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    else if (a == "--no-aec") cfg.aec = false;
    else if (a == "--gain") cfg.gain_db = std::atof(next("--gain"));
    else if (a == "--latch") cfg.start_latched = true;
    else if (a == "--test-signal") cfg.test_signal = true;
    else if (a == "--seconds") cfg.run_seconds = std::atoi(next("--seconds"));
    else if (a == "--channels") cfg.channels = std::atoi(next("--channels"));
    else if (a == "--no-ui") cfg.ui_port = 0;
    else if (a == "--ui-port") cfg.ui_port = static_cast<uint16_t>(std::atoi(next("--ui-port")));
    else if (a == "--open") cfg.open_browser = true;
    else if (a == "--no-open") cfg.open_browser = false;
    else {
      std::printf("ERROR: unknown argument %s\n\n", a.c_str());
      PrintUsage();
      return 2;
    }
  }

  LoadLocalEnv(&cfg, &meeting_arg);
  if (cfg.public_app_key.empty()) cfg.public_app_key = kDefaultPublicAppKey;

  std::printf("ZComms %s -- talkback panel\n\n", kAppVersion);

  // --- Control surface, FIRST ----------------------------------------------
  // The panel is the product's face, so it exists from the first moment --
  // including the join step, which lives in the panel rather than in a
  // console prompt. Everything the server's threads touch here is an atomic
  // or a mutex-guarded edge list applied later on the main thread.
  std::atomic<uint32_t> ui_talk_mask{0};
  std::atomic<int> side_req{-1}, aec_req{-1};
  std::atomic<int> gain_req{0};
  std::atomic<bool> gain_pending{false};
  std::atomic<bool> quit_req{false};
  std::mutex edge_m;
  std::vector<std::pair<int, bool>> latch_edges;
  std::vector<std::tuple<int, unsigned int, bool>> assign_edges;
  std::mutex join_m;
  std::string join_pending;
  std::string passcode_pending;

  std::unique_ptr<ControlServer> ui;
  if (cfg.ui_port != 0) {
    ui = std::make_unique<ControlServer>(
        kPanelHtml,
        [&](const std::string& verb, const std::string& arg) {
          const auto sp = arg.find(' ');
          const std::string a1 = sp == std::string::npos ? arg : arg.substr(0, sp);
          const std::string a2 = sp == std::string::npos ? "" : arg.substr(sp + 1);
          if (verb == "talk") {
            const int slot = std::atoi(a1.c_str());
            const bool on = a2 == "on";
            uint32_t m = ui_talk_mask.load();
            for (;;) {
              const uint32_t next = on ? (m | (1u << slot)) : (m & ~(1u << slot));
              if (ui_talk_mask.compare_exchange_weak(m, next)) break;
            }
          } else if (verb == "latch") {
            std::lock_guard<std::mutex> lock(edge_m);
            latch_edges.emplace_back(std::atoi(a1.c_str()), a2 == "on");
          } else if (verb == "assign") {
            const auto colon = a1.find(':');
            if (colon != std::string::npos) {
              std::lock_guard<std::mutex> lock(edge_m);
              assign_edges.emplace_back(
                  std::atoi(a1.substr(0, colon).c_str()),
                  static_cast<unsigned int>(
                      std::strtoul(a1.substr(colon + 1).c_str(), nullptr, 10)),
                  a2 == "on");
            }
          } else if (verb == "join") {
            // The whole argument is the pasted link (it may contain spaces).
            std::lock_guard<std::mutex> lock(join_m);
            join_pending = arg;
          } else if (verb == "passcode") {
            std::lock_guard<std::mutex> lock(join_m);
            passcode_pending = a1;
          } else if (verb == "sidetone") {
            side_req.store(a1 == "on" ? 1 : 0);
          } else if (verb == "aec") {
            aec_req.store(a1 == "on" ? 1 : 0);
          } else if (verb == "gain") {
            gain_req.store(std::atoi(a1.c_str()));
            gain_pending.store(true);
          } else if (verb == "quit") {
            quit_req.store(true);
          }
        });
    std::string ui_err;
    if (ui->Start(cfg.ui_port, &ui_err)) {
      const std::string url = "http://127.0.0.1:" + std::to_string(cfg.ui_port);
      std::printf("[ui] panel at %s\n", url.c_str());
      if (cfg.open_browser) {
        // The app's own window first (WebView2, panel at its designed
        // 1000x640); a borrowed browser frame only if the runtime is absent.
        // Closing the shell window is quitting the app.
        bool shown = false;
#ifdef ZCOMMS_HAVE_WEBVIEW2
        shown = StartShellWindow(url, 1000, 640,
                                 [&quit_req]() { quit_req.store(true); });
#endif
        if (!shown) OpenAppWindow(url);
      }
    } else {
      std::printf("WARNING: %s -- running without the panel\n", ui_err.c_str());
      ui.reset();
    }
  }

  // Ops log, panel-mirrored from the start.
  std::vector<std::string> ops_log;
  const auto log_op = [&](const std::string& line) {
    std::printf("\n[zcomms] %s\n", line.c_str());
    ops_log.push_back(line);
    if (ops_log.size() > 5) ops_log.erase(ops_log.begin());
  };

  // Minimal state while there is no meeting yet: enough for the panel to
  // show the join card and any parse error.
  const auto publish_phase = [&](const std::string& phase,
                                 const std::string& status) {
    if (!ui) return;
    std::string j = "{\"phase\":\"" + JsonEscape(phase) + "\",";
    j += "\"status\":\"" + JsonEscape(status) + "\",";
    j += "\"meeting\":\"\",\"channels\":[],\"roster\":[],\"log\":[";
    bool first = true;
    for (const std::string& l : ops_log) {
      if (!first) j += ",";
      first = false;
      j += "\"" + JsonEscape(l) + "\"";
    }
    j += "]}";
    ui->PublishState(j);
  };

  // --- Session cycle --------------------------------------------------------
  // The app outlives its meetings. Acquire one, run it, and on any outcome
  // short of "quit" -- join failure, SDK conflict, meeting over -- come back
  // here with the panel alive and the reason in the ops log. (Before this
  // loop existed, failures exited the process, which from the operator's
  // chair looked like the app froze and vanished: the live bug of 2026-08-29,
  // InitSDK error 14 while OBS held the Zoom engine.)
  for (;;) {
  // --- Acquire a meeting ----------------------------------------------------
  for (;;) {
    if (!meeting_arg.empty() &&
        ParseMeetingArg(meeting_arg, &cfg.meeting_number,
                        &cfg.meeting_password)) {
      break;
    }
    if (!meeting_arg.empty()) {
      log_op("could not read a meeting id out of \"" + meeting_arg + "\"");
      meeting_arg.clear();
    }
    if (ui) {
      publish_phase("idle", "PASTE A MEETING LINK");
      if (quit_req.load()) return 0;
      Sleep(100);
      std::lock_guard<std::mutex> lock(join_m);
      if (!join_pending.empty()) {
        meeting_arg = join_pending;
        join_pending.clear();
      }
    } else {
      // Headless fallback: the console conversation.
      std::printf("Paste the Zoom meeting link (or meeting ID) and press "
                  "Enter:\n> ");
      std::string line;
      if (!std::getline(std::cin, line) || line.empty()) {
        PrintUsage();
        return 2;
      }
      meeting_arg = line;
    }
  }
  publish_phase("joining", "JOINING MEETING");
  std::printf("  USE A HEADSET or keep ECHO CANCEL on.\n\n");

  // One meeting session, start to finish. Runs as a lambda so every failure
  // is a return to the session cycle, never a process exit. Returns:
  //   0  operator quit / scripted run elapsed -> exit the app
  //   1  meeting ended normally              -> back to the join card
  //  -1  bring-up failed                     -> back to the join card
  const auto session = [&]() -> int {
  // --- Zoom bring-up --------------------------------------------------------
  ZoomClient zoom;
  std::string err;
  if (!zoom.Init(&err)) {
    log_op(err + " -- " + SdkConflictHint());
    return -1;
  }
  if (!zoom.Authenticate(cfg.public_app_key, "", "", 30000, &err)) {
    log_op("sign-in failed: " + err);
    zoom.Cleanup();
    return -1;
  }
  publish_phase("joining", "JOINING -- ADMIT \"" + cfg.display_name +
                               "\" IF A WAITING ROOM PROMPTS");
  std::printf("[zoom] joining meeting %llu as \"%s\" -- admit it if a waiting "
              "room is on\n",
              (unsigned long long)cfg.meeting_number, cfg.display_name.c_str());
  // The join tick: keep the panel honest while Join blocks, and run the
  // passcode conversation when the meeting demands one the operator didn't
  // paste (bare meeting IDs do this; full invite links carry the passcode).
  int last_pc_state = 0;
  ZOOM_SDK_NAMESPACE::MeetingStatus last_js =
      ZOOM_SDK_NAMESPACE::MEETING_STATUS_IDLE;
  const auto join_tick = [&]() {
    // Mirror real SDK status to the panel; "ADMIT..." while the SDK sits in
    // WAITING_FOR_HOST reads as a hang from the operator's chair.
    const ZOOM_SDK_NAMESPACE::MeetingStatus js = zoom.status();
    if (js != last_js && zoom.passcode_state() == 0) {
      last_js = js;
      if (js == ZOOM_SDK_NAMESPACE::MEETING_STATUS_WAITINGFORHOST) {
        publish_phase("joining", "WAITING FOR THE HOST TO START THE MEETING");
      } else if (js == ZOOM_SDK_NAMESPACE::MEETING_STATUS_IN_WAITING_ROOM) {
        publish_phase("joining", "IN THE WAITING ROOM -- ADMIT \"" +
                                     cfg.display_name + "\"");
      } else if (js == ZOOM_SDK_NAMESPACE::MEETING_STATUS_RECONNECTING) {
        publish_phase("joining", "RECONNECTING...");
      }
    }
    const int pc = zoom.passcode_state();
    if (pc != 0 && pc != last_pc_state) {
      publish_phase("joining", pc == 2 ? "WRONG PASSCODE -- TRY AGAIN"
                                       : "ENTER THE MEETING PASSCODE");
    }
    last_pc_state = pc;
    if (pc != 0) {
      std::string p;
      {
        std::lock_guard<std::mutex> lock(join_m);
        p.swap(passcode_pending);
      }
      if (!p.empty()) {
        publish_phase("joining", "CHECKING PASSCODE...");
        zoom.SubmitPasscode(p);
      }
    }
  };
  if (!zoom.Join(cfg.meeting_number, cfg.meeting_password, cfg.display_name,
                 600000, &err, join_tick)) {
    log_op("could not join: " + err);
    zoom.Cleanup();
    return -1;
  }
  std::printf("[zoom] in the meeting\n");
  if (!zoom.JoinVoip(&err)) {
    std::printf("WARNING: %s\n", err.c_str());
  }

  Roster roster;
  roster.Attach(zoom.GetParticipantsController());

  // --- Channel bring-up (host/co-host retry, as proven in the spike) --------
  const int n_channels =
      cfg.channels < 1 ? 1
                       : (cfg.channels > TalkbackChannels::kMaxChannels
                              ? TalkbackChannels::kMaxChannels
                              : cfg.channels);
  TalkbackChannels bank(zoom.GetTalkbackController());
  if (!bank.meeting_supports_talkback()) {
    log_op("this meeting does not support talkback");
    zoom.Leave();
    zoom.Cleanup();
    return -1;
  }
  bool created = false;
  for (int attempt = 0; attempt < 120 && !created; ++attempt) {
    if (bank.channels_ready() == 0) {
      // Request the whole bank in one call; CreateChannel is rate-limited.
      if (!bank.CreateChannels(n_channels, &err)) {
        std::printf("[talkback] %s\n", err.c_str());
      }
    }
    for (int i = 0; i < 100 && bank.channels_ready() < n_channels; ++i) {
      zoom.Pump(100);
    }
    created = bank.channels_ready() >= n_channels;
    if (!created) {
      publish_phase("joining", "MAKE \"" + cfg.display_name +
                                   "\" HOST OR CO-HOST (Participants -> More)");
      std::printf(">>> promote \"%s\" to HOST or CO-HOST in the participant "
                  "list (retrying, %d)\n",
                  cfg.display_name.c_str(), attempt + 1);
      zoom.Pump(5000);
    }
  }
  if (!created) {
    log_op("could not create talkback channels (never granted host/co-host?)");
    zoom.Leave();
    zoom.Cleanup();
    return -1;
  }
  bank.SetBackgroundVolumeAll(0.2f);  // duck, don't erase, the meeting
  std::printf("[zoom] %d channel(s) up\n", n_channels);

  // --- Audio path -----------------------------------------------------------
  // Normal mode: the engine, mic through gain/limiter/PTT into the channel.
  // Test mode: an internally generated beep pattern through the same ring and
  // pacer, so the paced TX path is exercised identically -- only the source
  // differs.
  ChannelBankSink sink(&bank);
  std::unique_ptr<AudioEngine> engine;
  std::unique_ptr<FrameRing> test_ring;
  std::unique_ptr<TxPacer> test_pacer;

  std::unique_ptr<SignalGenerator> test_gen_src;
  if (cfg.test_signal) {
    // Spike A's probe: alternating up/down chirp bursts on a comfort-noise
    // bed, every second. Chosen over tones because the far end of the e2e
    // test detects by matched filter, and a chirp stays detectable through
    // broadband audio (music, a live mic) that drowns a tone's energy ratio.
    std::printf("[audio] TEST SIGNAL mode: chirp probe (matched-filter "
                "detectable)\n");
    test_ring = std::make_unique<FrameRing>(50);
    test_pacer = std::make_unique<TxPacer>(test_ring.get(), &sink, nullptr);
    SignalParams sp;
    sp.period_ms = 1000.0;  // denser than the spike: detection, not latency
    test_gen_src = std::make_unique<SignalGenerator>(test_ring.get(), sp);
    test_gen_src->Start();
    test_pacer->Start(3, 500);
  } else {
    EngineConfig ecfg;
    ecfg.capture_device = cfg.mic_device;
    ecfg.monitor_device = cfg.monitor_device;
    ecfg.monitor_enabled = cfg.sidetone;
    ecfg.input_gain_db = cfg.gain_db;
    ecfg.aec = cfg.aec;
    engine = std::make_unique<AudioEngine>(ecfg, &sink);
    if (!engine->Start(&err)) {
      log_op("audio engine failed: " + err);
      zoom.Leave();
      zoom.Cleanup();
      return -1;
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
  // (Control surface + ops log were brought up before the meeting existed;
  // from here on the main loop applies their edges.)
  log_op("station up -- " + std::to_string(n_channels) + " channel(s)");

  // Membership intent, per (slot, uid). Auto-policy: capable participants
  // land on CH 1; the panel moves them. Intent is healed against confirmed
  // membership on the housekeeping cadence.
  std::set<std::pair<int, unsigned int>> intent;
  std::set<unsigned int> auto_assigned;
  std::set<unsigned int> warned_no_talkback;
  int64_t next_house_ns = 0;
  // All-call latch (SPACE latches every channel); per-channel latch mask.
  uint32_t latch_mask = cfg.start_latched ? 0xFFFFFFFFu : 0u;
  bool quit = false;
  bool talking = false;
  double gain_db = cfg.gain_db;
  bool sidetone_on = cfg.sidetone;
  const int64_t end_ns =
      cfg.run_seconds > 0
          ? NowNs() + static_cast<int64_t>(cfg.run_seconds) * 1'000'000'000LL
          : 0;

  // Main-loop watchdog: the one AppHangB1 on record stalled this loop with
  // no output and no crash dump -- the worst kind of failure to diagnose
  // after the fact. The watchdog cannot fix a hang, but it converts "the app
  // silently stopped" into a loud, timestamped record of exactly when the
  // loop last ran, in the console and therefore in any captured log.
  std::atomic<int64_t> heartbeat_ns{NowNs()};
  std::atomic<bool> watchdog_running{true};
  std::thread watchdog([&heartbeat_ns, &watchdog_running]() {
    bool reported = false;
    while (watchdog_running.load()) {
      Sleep(1000);
      const int64_t age_ms = (NowNs() - heartbeat_ns.load()) / 1'000'000;
      if (age_ms > 5000 && !reported) {
        reported = true;
        std::printf("\n[watchdog] MAIN LOOP STALLED for %lld ms -- if this "
                    "persists the app is hung (known: AppHangB1, see "
                    "CLAUDE.md)\n",
                    static_cast<long long>(age_ms));
      } else if (age_ms < 1000) {
        reported = false;
      }
    }
  });

  while (!quit && zoom.in_meeting()) {
    if (end_ns != 0 && NowNs() >= end_ns) break;
    heartbeat_ns.store(NowNs());
    zoom.Pump(30);

    // Host duties + membership healing, on a coarse cadence. Invite anyone
    // not yet invited who supports talkback (the web client does not --
    // inviting it fails with INVALID_PARAMETER); Zoom drops leavers on its
    // own, and a rejoin arrives as a brand-new user id (plan §5). Retried on
    // a timer rather than only on roster changes, so a transient failure
    // heals instead of sticking. (Session-scoped, not static: user ids are
    // meeting-scoped and recycled, so state must die with the session.)
    roster.ConsumeDirty();
    if (NowNs() >= next_house_ns) {
      next_house_ns = NowNs() + 2'000'000'000LL;
      zoom.AdmitAllWaiting();  // no-op unless we are host

      // Default policy: a new capable participant lands on CH 1.
      for (const RosterMember& m : roster.others()) {
        if (!m.supports_talkback) {
          if (warned_no_talkback.insert(m.user_id).second) {
            log_op(m.name + " cannot receive talkback (web client) -- skipped");
          }
          continue;
        }
        if (auto_assigned.insert(m.user_id).second) {
          intent.insert({0, m.user_id});
          log_op(m.name + " -> CH 1");
        }
      }

      // Heal intent against confirmed membership: invite what should be in
      // and is not, remove what should not be and is. One pass, idempotent,
      // and re-run every cadence so transients fix themselves.
      const std::vector<ChannelState> snap = bank.Snapshot();
      for (int s = 0; s < static_cast<int>(snap.size()); ++s) {
        for (const RosterMember& m : roster.others()) {
          const bool want = intent.count({s, m.user_id}) != 0;
          const bool have =
              snap[static_cast<size_t>(s)].members.count(m.user_id) != 0;
          if (want && !have && m.supports_talkback) {
            if (!bank.Invite(s, m.user_id, &err)) {
              log_op("CH " + std::to_string(s + 1) + " invite failed: " + err);
            }
          } else if (!want && have) {
            if (!bank.Remove(s, m.user_id, &err)) {
              log_op("CH " + std::to_string(s + 1) + " remove failed: " + err);
            }
          }
        }
      }
    }

    // Assignment edges from the panel mutate intent; the healer does the rest.
    {
      std::vector<std::pair<int, bool>> ledges;
      std::vector<std::tuple<int, unsigned int, bool>> aedges;
      {
        std::lock_guard<std::mutex> lock(edge_m);
        ledges.swap(latch_edges);
        aedges.swap(assign_edges);
      }
      for (const auto& [slot, on] : ledges) {
        if (on) latch_mask |= 1u << slot;
        else latch_mask &= ~(1u << slot);
      }
      for (const auto& [slot, uid, on] : aedges) {
        if (on) intent.insert({slot, uid});
        else intent.erase({slot, uid});
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
      const std::vector<ChannelState> snap = bank.Snapshot();
      const uint32_t keys = bank.key_mask();
      std::string j = "{\"phase\":\"up\",";
      j += "\"meeting\":\"" + std::to_string(cfg.meeting_number) + "\",";
      j += "\"status\":\"" + status + "\",";
      j += std::string("\"talking\":") + (talking ? "true," : "false,");
      j += std::string("\"sidetone\":") + (sidetone_on ? "true," : "false,");
      j += std::string("\"aec\":") +
           ((engine && engine->aec_enabled()) ? "true," : "false,");
      j += "\"gain\":" + std::to_string(static_cast<int>(gain_db)) + ",";
      char pk[32];
      std::snprintf(pk, sizeof(pk), "%.4f", peak);
      j += std::string("\"peak\":") + pk + ",";
      j += "\"sends\":" + std::to_string(sends) + ",";
      j += "\"underruns\":" + std::to_string(unders) + ",";
      j += "\"fails\":" + std::to_string(bank.send_failures()) + ",";
      j += "\"channels\":[";
      bool first = true;
      for (int s = 0; s < static_cast<int>(snap.size()); ++s) {
        const ChannelState& c = snap[static_cast<size_t>(s)];
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"" + JsonEscape(c.name) + "\",";
        j += std::string("\"ready\":") + (c.ready ? "true," : "false,");
        j += "\"listeners\":" + std::to_string(c.listeners) + ",";
        j += std::string("\"keyed\":") +
             (((keys >> s) & 1u) ? "true," : "false,");
        j += std::string("\"latched\":") +
             (((latch_mask >> s) & 1u) ? "true}" : "false}");
      }
      j += "],\"roster\":[";
      first = true;
      for (const RosterMember& m : roster.others()) {
        if (!first) j += ",";
        first = false;
        j += "{\"name\":\"" + JsonEscape(m.name) + "\",";
        j += "\"uid\":" + std::to_string(m.user_id) + ",";
        j += std::string("\"tb\":") + (m.supports_talkback ? "true," : "false,");
        j += "\"chans\":[";
        bool cf = true;
        for (int s = 0; s < static_cast<int>(snap.size()); ++s) {
          if (!cf) j += ",";
          cf = false;
          const bool on =
              snap[static_cast<size_t>(s)].members.count(m.user_id) != 0 ||
              intent.count({s, m.user_id}) != 0;
          j += on ? "true" : "false";
        }
        j += "]}";
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

    // Apply remaining control-surface edges.
    if (quit_req.load()) quit = true;
    {
      const int sr = side_req.exchange(-1);
      if (sr >= 0) {
        sidetone_on = sr == 1;
        if (engine) engine->SetSidetoneEnabled(sidetone_on);
      }
      const int ar = aec_req.exchange(-1);
      if (ar >= 0 && engine) engine->SetAecEnabled(ar == 1);
      if (gain_pending.exchange(false)) {
        gain_db = static_cast<double>(gain_req.load());
        if (engine) engine->SetInputGainDb(gain_db);
      }
    }

    // Keying. SPACE is all-call (every channel), the panel keys channels
    // individually, latch is per-channel state. The bank's key mask is the
    // single routing truth the TX path reads.
    const bool space_down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const uint32_t all = (n_channels >= 32) ? 0xFFFFFFFFu
                                            : ((1u << n_channels) - 1u);
    const uint32_t keys =
        (latch_mask | ui_talk_mask.load() | (space_down ? all : 0u)) & all;
    for (int s = 0; s < n_channels; ++s) {
      bank.SetKey(s, ((keys >> s) & 1u) != 0);
    }
    talking = keys != 0;
    // The engine's envelope opens when any channel is keyed; per-channel
    // routing happens at the sink. A channel keyed mid-speech joins at full
    // level, which is how hardware panels behave.
    if (engine) engine->SetTalk(talking);

    while (_kbhit()) {
      const int c = _getch();
      switch (c) {
        case 'q': case 'Q': quit = true; break;
        case 'l': case 'L':
          // Console latch toggles all-call latch.
          latch_mask = (latch_mask & all) == all ? 0u : all;
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
      std::printf("\r  [%s] %-6s  keys %02x  gain %+.0f dB  underrun %llu   ",
                  MeterBar(s.capture_peak).c_str(), (talking ? "TALK" : ""),
                  keys, gain_db, (unsigned long long)s.pacer.underruns);
    } else if (test_pacer) {
      const PacerStats p = test_pacer->stats();
      std::printf("\r  [test-signal] keys %02x  ready %d/%d  sends %llu  "
                  "fail %llu   ",
                  keys, bank.channels_ready(), n_channels,
                  (unsigned long long)p.sends,
                  (unsigned long long)bank.send_failures());
    }
  }

  // --- Teardown -------------------------------------------------------------
  // Decide the outcome before tearing down: quit (or a scripted run elapsing)
  // exits the app; the meeting ending underneath us goes back to the join
  // card with the panel still alive.
  const bool app_exit = quit || (end_ns != 0 && NowNs() >= end_ns);
  watchdog_running.store(false);
  if (watchdog.joinable()) watchdog.join();
  std::printf("\n[zoom] leaving...\n");
  if (engine) {
    engine->SetTalk(false);
    Sleep(100);  // let the release ramp finish before the engine stops
    engine->Stop();
  }
  if (test_pacer) test_pacer->Stop();
  if (test_gen_src) test_gen_src->Stop();
  zoom.Leave();
  zoom.Cleanup();
  return app_exit ? 0 : 1;
  };  // end of session lambda

  const int session_rc = session();
  if (!ui) return session_rc < 0 ? 1 : 0;  // headless runs one session
  if (session_rc == 0) return 0;           // operator quit / run elapsed
  if (session_rc > 0) log_op("meeting ended");
  // Back to the join card for the next meeting.
  meeting_arg.clear();
  cfg.meeting_number = 0;
  cfg.meeting_password.clear();
  }  // session cycle
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
