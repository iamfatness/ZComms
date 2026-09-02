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
#include <io.h>
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
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "app_identity.h"
#include "audio_defs.h"
#include "channel_mix.h"
#include "crash_trap.h"
#include "extern_feed.h"
#include "signal_gate.h"
#include "clock.h"
#include "control_server.h"
#include "zoom_oauth.h"
#ifdef ZCOMMS_HAVE_WEBVIEW2
#include "shell_window.h"
#endif
#include "devices.h"
#include "engine.h"
#include "frame_ring.h"
#include "generator.h"
#include "breakout.h"
#include "chat_signals.h"
#include "duck_plan.h"
#include "reach.h"
#include "room_plan.h"
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

// True when an interactive console exists for hotkeys (_kbhit/_getch).
bool g_console_keys = false;

// The exe is a Windows-subsystem app -- a real windowed app that never
// creates a console of its own (owner, 2026-08-30: the console window
// alongside the panel read as not-a-real-app). The printf diagnostic
// stream is still load-bearing, so it lands somewhere useful, decided once
// at startup in priority order: an already-redirected stdout is honored
// (scripted runs and pipes), a parent terminal is attached (dev runs print
// where they were typed), and an Explorer launch writes a dated log file
// under %APPDATA%\ZComms\logs so the instruments survive with no console
// anywhere. The panel's ops ticker remains the operator surface.
void BindStdio() {
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE) {
    // Inherited or redirected by the launcher: leave it alone.
    g_console_keys = GetConsoleWindow() != nullptr;
    return;
  }
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    // The parent shell's prompt has already returned (it does not wait for
    // a GUI-subsystem exe); start our output on a fresh line.
    std::printf("\n");
    g_console_keys = true;
    return;
  }
  char appdata[MAX_PATH] = {};
  if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) == 0) {
    return;  // no console, no %APPDATA%: output goes nowhere, app still runs
  }
  std::string dir = std::string(appdata) + "\\ZComms";
  CreateDirectoryA(dir.c_str(), nullptr);
  dir += "\\logs";
  CreateDirectoryA(dir.c_str(), nullptr);
  SYSTEMTIME st;
  GetLocalTime(&st);
  char name[64];
  std::snprintf(name, sizeof(name), "\\zcomms-%04u%02u%02u-%02u%02u%02u.log",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::string path = dir + name;
  FILE* f = nullptr;
  // Both streams in APPEND mode: the timestamped name is fresh each launch,
  // and "a" makes every write land at true EOF. The original "w"/"a" split
  // gave the two streams independent file positions, which shuffled a
  // FATAL (stderr) line into the middle of the stdout stream on the first
  // field crash log -- chronology in a crash log is evidence, keep it.
  freopen_s(&f, path.c_str(), "a", stdout);
  freopen_s(&f, path.c_str(), "a", stderr);
}

struct AppConfig {
  std::string public_app_key;
  // Anonymous guest join under the bare public app key. OFF by default:
  // the product joins as the signed-in operator (OAuth broker + ZAK), which
  // is what CoreVideo does and what lifts the cross-account 504 refusal.
  // The flag remains for scripted same-account test runs.
  bool anon = false;
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
  // Send each auto-assigned talent a private courtesy chat ("you are on
  // talkback CH n"). OFF by default: in a real production (Office Hours,
  // 2026-08-30) the bring-up messaged the whole meeting, audience included
  // -- one line per participant, each at join/co-host grant. The panel's
  // explicit `notify <slot>` verb is always available regardless.
  bool announce = false;
  int run_seconds = 0;  // 0 = run until Q
  // The browser panel / control API (plan §6.4). 0 disables.
  uint16_t ui_port = 7350;
  // The panel opens itself by default: a packaged app's face is the panel,
  // and a double-clicked exe that silently listens on a port looks broken.
  bool open_browser = true;
  // Talkback channels to create (1..16). The whole bank by default: every
  // panelist gets their OWN standing channel (their key = their name, a
  // direct line), all-call spans them, and the chips regroup people. Keying
  // must only ever SELECT -- provisioning anything on a press costs the
  // first syllable (CoreVideo talkback, live-measured).
  int channels = 16;
};

// Latched extern feeds: one per talkback slot, a channel (or pair) of a
// multichannel device carried into that channel continuously -- the larger
// system's mix, with Zoom as the last mile (spec docs/plans/
// 2026-09-01-extern-feeds.md). App-lifetime, not session-scoped: feeds are
// rig plumbing and survive the session cycle; feeds.env survives the app.
class FeedBank {
 public:
  static constexpr int kSlots = TalkbackChannels::kMaxChannels;

  struct Status {
    int slot = -1;
    std::string spec;
    double gain_db = 0.0;
    bool latch = false;
    bool dev_ok = false;
    int peak = 0;
  };

  // Control/main thread. Returns the ops line describing what happened.
  std::string Apply(const std::string& arg) {
    const auto sp1 = arg.find(' ');
    const std::string op = sp1 == std::string::npos ? arg : arg.substr(0, sp1);
    const std::string rest = sp1 == std::string::npos ? "" : arg.substr(sp1 + 1);
    const auto sp2 = rest.find(' ');
    const int slot = std::atoi(
        (sp2 == std::string::npos ? rest : rest.substr(0, sp2)).c_str());
    const std::string val = sp2 == std::string::npos ? "" : rest.substr(sp2 + 1);
    if (slot < 0 || slot >= kSlots) return "feed: bad channel";
    const std::string ch = "CH " + std::to_string(slot + 1);

    if (op == "set") {
      FeedConfig c;
      if (!ParseFeedSpec(val, &c)) {
        return "feed: could not read \"" + val + "\" (device:ch or device:ch-ch)";
      }
      std::string err;
      if (!Set(slot, c, &err)) return "feed " + ch + ": " + err;
      SaveEnv();
      return "feed " + ch + " <- " + FormatFeedSpec(slots_[slot].cfg) + " (" +
             device_name(slot) + ")";
    }
    if (op == "latch") {
      if (!SetLatch(slot, val == "on")) return "feed " + ch + ": no feed set";
      SaveEnv();
      return "feed " + ch + (val == "on" ? " LATCHED" : " unlatched");
    }
    if (op == "gain") {
      if (!SetGain(slot, std::atof(val.c_str()))) {
        return "feed " + ch + ": no feed set";
      }
      SaveEnv();
      return "feed " + ch + " gain " + val + " dB";
    }
    if (op == "off") {
      Off(slot);
      SaveEnv();
      return "feed " + ch + " removed";
    }
    return "feed: unknown op \"" + op + "\" (set|latch|gain|off)";
  }

  bool Set(int slot, const FeedConfig& cfg, std::string* err) {
    std::lock_guard<std::mutex> lock(m_);
    Slot& s = slots_[slot];
    if (s.dev) s.dev->Stop();
    s.dev.reset();
    FeedConfig c = cfg;
    // Re-setting a feed keeps its latch/gain unless the new spec says
    // otherwise -- the common case is repointing a device, not a reset.
    if (s.chain) {
      c.latch = s.cfg.latch;
      c.gain_db = s.cfg.gain_db;
    }
    s.cfg = c;
    s.chain = std::make_unique<FeedChain>(c);
    s.gate = std::make_unique<SignalGate>(-50.0, 800, kSampleRate);
    s.dev = std::make_unique<MultiCaptureDevice>();
    FeedChain* chain = s.chain.get();
    s.dev_ok = s.dev->Start(
        c.device,
        [chain](const float* in, int frames, int nch) {
          chain->PushInterleaved(in, frames, nch);
        },
        err);
    if (!s.dev_ok) {
      s.dev.reset();
      return false;
    }
    RefreshMasks();
    return true;
  }

  void Off(int slot) {
    std::lock_guard<std::mutex> lock(m_);
    Slot& s = slots_[slot];
    if (s.dev) s.dev->Stop();
    s.dev.reset();
    s.chain.reset();
    s.gate.reset();
    s.cfg = FeedConfig{};
    s.dev_ok = false;
    RefreshMasks();
  }

  bool SetLatch(int slot, bool on) {
    std::lock_guard<std::mutex> lock(m_);
    Slot& s = slots_[slot];
    if (!s.chain) return false;
    s.cfg.latch = on;
    s.chain->SetLatch(on);
    if (!on) draining_mask_.fetch_or(1u << slot);
    RefreshMasks();
    return true;
  }

  bool SetGain(int slot, double db) {
    std::lock_guard<std::mutex> lock(m_);
    Slot& s = slots_[slot];
    if (!s.chain) return false;
    s.cfg.gain_db = db;
    s.chain->SetGainDb(db);
    return true;
  }

  std::vector<Status> Snapshot() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<Status> out;
    for (int i = 0; i < kSlots; ++i) {
      const Slot& s = slots_[i];
      if (!s.chain) continue;
      Status st;
      st.slot = i;
      st.spec = FormatFeedSpec(s.cfg);
      st.gain_db = s.cfg.gain_db;
      st.latch = s.cfg.latch;
      st.dev_ok = s.dev_ok && s.dev && s.dev->running();
      st.peak = s.chain->peak();
      out.push_back(st);
    }
    return out;
  }

  // TX pacer thread. Slots the sink must service this tick: latched feeds
  // plus chains still draining their unlatch ramp-out tail (cutting the
  // tail would put the click back that the envelope exists to remove).
  uint32_t send_targets() const {
    return latched_mask_.load() | draining_mask_.load();
  }

  // Pull slot's frame. True = `out` is valid (audio, or silence covering a
  // latched underrun -- the pacer law: starvation is counted, never a
  // stall). False = this slot has no feed content this tick.
  bool PullFrame(int slot, int16_t* out) {
    std::lock_guard<std::mutex> lock(m_);
    Slot& s = slots_[slot];
    if (!s.chain) {
      draining_mask_.fetch_and(~(1u << slot));
      return false;
    }
    if (s.chain->PullFrame(out)) {
      if (s.gate && s.gate->Update(out, kFrameSamples)) {
        active_mask_.fetch_or(1u << slot);
      } else {
        active_mask_.fetch_and(~(1u << slot));
      }
      return true;
    }
    if (s.gate) s.gate->Update(nullptr, kFrameSamples);
    active_mask_.fetch_and(~(1u << slot));
    if (!s.cfg.latch) {
      // Drained: the ramp-out tail is gone, stop servicing the slot.
      draining_mask_.fetch_and(~(1u << slot));
      return false;
    }
    if (s.dev_ok && s.dev && s.dev->running()) {
      std::memset(out, 0, static_cast<size_t>(kFrameSamples) * sizeof(int16_t));
      underruns_.fetch_add(1);
      return true;
    }
    return false;  // device dead: no feed rather than eternal silence
  }

  // Slots whose feed is ACTUALLY carrying audio (SignalGate) -- the duck
  // planner's input, never latch state (the ZoomISO refinement).
  uint32_t active_mask() const { return active_mask_.load(); }
  uint64_t underruns() const { return underruns_.load(); }

  void LoadEnv(const std::function<void(const std::string&)>& log) {
    std::ifstream f(EnvPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos || line.rfind("feed", 0) != 0) continue;
      const int slot = std::atoi(line.substr(4, eq - 4).c_str());
      FeedConfig c;
      if (slot < 0 || slot >= kSlots || !ParseFeedLine(line.substr(eq + 1), &c))
        continue;
      std::string err;
      if (Set(slot, c, &err)) {
        // Set() preserves prior latch/gain only when re-setting; first set
        // takes them from the parsed config -- restore explicitly.
        SetLatch(slot, c.latch);
        SetGain(slot, c.gain_db);
        log("feed CH " + std::to_string(slot + 1) + " restored: " +
            FormatFeedSpec(c) + (c.latch ? " (latched)" : ""));
      } else {
        log("feed CH " + std::to_string(slot + 1) + " NOT restored: " + err);
      }
    }
  }

  void SaveEnv() {
    // m_ deliberately not held: callers hold no lock and the writes race at
    // worst with another verb, which last-writer-wins resolves.
    std::ofstream f(EnvPath(), std::ios::trunc);
    if (!f) return;
    f << "# ZComms extern feeds -- written by the app on every feed change\n";
    for (int i = 0; i < kSlots; ++i) {
      std::lock_guard<std::mutex> lock(m_);
      if (!slots_[i].chain) continue;
      f << "feed" << i << "=" << FormatFeedLine(slots_[i].cfg) << "\n";
    }
  }

 private:
  struct Slot {
    FeedConfig cfg;
    std::unique_ptr<FeedChain> chain;
    std::unique_ptr<MultiCaptureDevice> dev;
    std::unique_ptr<SignalGate> gate;
    bool dev_ok = false;
  };

  std::string device_name(int slot) const {
    return slots_[slot].dev ? slots_[slot].dev->device_name() : "?";
  }

  void RefreshMasks() {
    uint32_t latched = 0;
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].chain && slots_[i].cfg.latch) latched |= 1u << i;
    }
    latched_mask_.store(latched);
  }

  static std::string EnvPath() {
    char appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) == 0) {
      return "feeds.env";
    }
    const std::string dir = std::string(appdata) + "\\ZComms";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\feeds.env";
  }

  mutable std::mutex m_;
  Slot slots_[kSlots];
  std::atomic<uint32_t> latched_mask_{0};
  std::atomic<uint32_t> draining_mask_{0};
  std::atomic<uint32_t> active_mask_{0};
  std::atomic<uint64_t> underruns_{0};
};

// The seam between the paced TX thread and the channel bank. Composes each
// slot's outgoing frame -- operator voice when keyed, plus that slot's
// latched extern feed, barge-ducked while the voice ACTUALLY carries audio
// (ChannelMix) -- and sends one distinct mix per slot. "Nothing keyed and
// nothing latched" is a normal quiet state, not a send failure.
class ChannelBankSink : public FrameSink {
 public:
  ChannelBankSink(TalkbackChannels* bank, FeedBank* feeds)
      : bank_(bank), feeds_(feeds), mix_(50.0, kSampleRate),
        voice_gate_(-50.0, 800, kSampleRate) {}
  bool CanSend() override { return bank_->channels_ready() > 0; }
  bool Send(const int16_t* pcm, int samples) override {
    // The peak of what actually leaves for Zoom, post-envelope: the number
    // that separates "transmitting your voice" from "keyed but shipping
    // silence" (an envelope/mute defect) at a glance.
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
      const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a > peak) peak = a;
    }
    tx_peak_.store(peak);
    voice_active_.store(voice_gate_.Update(pcm, samples));

    const uint32_t keys = bank_->key_mask();
    const uint32_t ftargets = feeds_ != nullptr ? feeds_->send_targets() : 0;
    const uint32_t ready = bank_->ready_mask();
    const uint32_t targets = (keys | ftargets) & ready;
    if (targets == 0) return true;

    const bool va = voice_active_.load();
    int sent = 0;
    bool failed = false;
    int16_t fbuf[kFrameSamples];
    int16_t obuf[kFrameSamples];
    for (int s = 0; s < TalkbackChannels::kMaxChannels; ++s) {
      if (((targets >> s) & 1u) == 0) continue;
      const bool keyed = ((keys >> s) & 1u) != 0;
      const int16_t* fptr = nullptr;
      if (((ftargets >> s) & 1u) != 0 && feeds_->PullFrame(s, fbuf)) {
        fptr = fbuf;
      }
      if (!mix_.Compose(s, keyed ? pcm : nullptr, fptr, va, samples, obuf)) {
        continue;
      }
      if (bank_->SendToSlot(s, obuf, samples)) {
        ++sent;
      } else {
        failed = true;
      }
    }
    return sent > 0 || !failed;
  }
  // 0..32767; only meaningful while something is keyed.
  int tx_peak() const { return tx_peak_.load(); }
  // The voice SignalGate: true while the operator's chain actually carries
  // audio. Feeds the duck planner alongside FeedBank::active_mask().
  bool voice_active() const { return voice_active_.load(); }

 private:
  TalkbackChannels* bank_;
  FeedBank* feeds_;
  ChannelMix mix_;
  SignalGate voice_gate_;
  std::atomic<int> tx_peak_{0};
  std::atomic<bool> voice_active_{false};
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
  --announce           Chat each auto-assigned talent a private "you are on
                       talkback CH n" notice. Default: silent bring-up (the
                       panel's NOTIFY button always works either way).

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
      for (const auto& d : ListCaptureDevices()) {
        char ch[32];
        if (d.channels) std::snprintf(ch, sizeof ch, "%d ch", d.channels);
        else std::snprintf(ch, sizeof ch, "channels unknown");
        std::printf("  %s%s  [%s]\n", d.name.c_str(),
                    d.is_default ? "  (default)" : "", ch);
      }
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
    else if (a == "--announce") cfg.announce = true;
    else if (a == "--seconds") cfg.run_seconds = std::atoi(next("--seconds"));
    else if (a == "--channels") cfg.channels = std::atoi(next("--channels"));
    else if (a == "--no-ui") cfg.ui_port = 0;
    else if (a == "--ui-port") cfg.ui_port = static_cast<uint16_t>(std::atoi(next("--ui-port")));
    else if (a == "--anon") cfg.anon = true;
    else if (a == "--open") cfg.open_browser = true;
    else if (a == "--no-open") cfg.open_browser = false;
    else if (a == "--selftest-crash") {
      // Hidden (not in usage): proves the crash trap end-to-end -- FATAL
      // line in the log AND the message box -- on any machine, including
      // one where only the screen can leave the room.
      const std::string mode = next("--selftest-crash");
      std::printf("[crash-trap] selftest: %s\n", mode.c_str());
      if (mode == "throw") throw std::out_of_range("crash-trap selftest");
      if (mode == "abort") std::abort();
      if (mode == "av") *static_cast<volatile int*>(nullptr) = 1;
      if (mode == "invalidparam") {
        // This one must SURVIVE: the route is non-fatal by policy. _close
        // on a bad descriptor is documented to invoke the handler, then
        // fail with EBADF.
        _close(-99);
        std::printf("[crash-trap] survived invalid parameter, count=%u\n",
                    InvalidParameterCount());
        return InvalidParameterCount() == 1 ? 0 : 2;
      }
      std::printf("ERROR: --selftest-crash takes throw|abort|av|invalidparam\n");
      return 2;
    }
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
  std::atomic<bool> ui_allcall{false};  // the panel's ALL CALL key
  std::atomic<int> side_req{-1}, aec_req{-1}, tone_req{-1};
  std::atomic<int> gain_req{0};
  std::atomic<bool> gain_pending{false};
  std::atomic<bool> quit_req{false};
  std::atomic<bool> leave_req{false};  // leave the meeting, keep the app
  std::mutex edge_m;
  std::vector<std::pair<int, bool>> latch_edges;
  std::vector<std::tuple<int, unsigned int, bool>> assign_edges;
  std::vector<std::string> bo_cmds;
  std::vector<std::pair<int, bool>> cue_edges;  // chat cue signals to a slot
  std::vector<int> notify_edges;                // assignment notices to a slot
  std::vector<std::string> feed_cmds;           // extern-feed verbs, main-thread applied
  std::mutex join_m;
  std::string join_pending;
  std::string passcode_pending;
  std::mutex dev_m;
  std::string mic_pending, out_pending;  // device switches from settings
  std::string room_pending;              // breakout-room move from settings

  // The operator's Zoom session. signed_in mirrors the token store so the
  // idle loop can gate the join card without a disk read per tick; --anon
  // short-circuits it for scripted same-account runs.
  ZoomOAuth oauth(cfg.ui_port != 0 ? cfg.ui_port : 7350);
  std::atomic<bool> signed_in{cfg.anon || oauth.signed_in()};

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
          } else if (verb == "tone") {
            tone_req.store(a1 == "on" ? 1 : 0);
          } else if (verb == "room") {
            std::lock_guard<std::mutex> lock(dev_m);
            room_pending = arg;
          } else if (verb == "cue") {
            // Structured cue signal to every member of a slot's channel.
            std::lock_guard<std::mutex> lock(edge_m);
            cue_edges.emplace_back(std::atoi(a1.c_str()), a2 == "on");
          } else if (verb == "notify") {
            // Human-readable you-are-on-channel notice, private chat.
            std::lock_guard<std::mutex> lock(edge_m);
            notify_edges.push_back(std::atoi(a1.c_str()));
          } else if (verb == "bo") {
            // Sub-production commands, parsed on the main thread:
            //   bo layout <room>:<p>,<p>;<room>:<p>   bo apply
            //   bo start                              bo stop
            std::lock_guard<std::mutex> lock(edge_m);
            bo_cmds.push_back(arg);
          } else if (verb == "feed") {
            // feed set <slot> <device:ch[-ch2]> | latch <slot> on|off |
            // gain <slot> <db> | off <slot>. Applied on the main thread:
            // Set opens a capture device.
            std::lock_guard<std::mutex> lock(edge_m);
            feed_cmds.push_back(arg);
          } else if (verb == "setmic") {
            // Device names carry spaces; the whole argument is the name.
            std::lock_guard<std::mutex> lock(dev_m);
            mic_pending = arg;
          } else if (verb == "setout") {
            std::lock_guard<std::mutex> lock(dev_m);
            out_pending = arg;
          } else if (verb == "talkall") {
            ui_allcall.store(a1 == "on");
          } else if (verb == "latchall") {
            std::lock_guard<std::mutex> lock(edge_m);
            latch_edges.emplace_back(-1, a1 == "on");
          } else if (verb == "signin") {
            // System browser, never the panel WebView: the operator's Zoom
            // session (and their password manager) lives there.
            const std::string url = oauth.BeginSignIn();
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
          } else if (verb == "signout") {
            oauth.SignOut();
            signed_in.store(false);
          } else if (verb == "leave") {
            leave_req.store(true);
          } else if (verb == "quit") {
            quit_req.store(true);
          }
        });
    ui->SetOAuthHandler([&](const std::string& query) -> std::string {
      std::string cb_err;
      if (oauth.HandleCallback(query, &cb_err)) {
        signed_in.store(true);
        return "";
      }
      return cb_err;
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
        if (!shown) {
          OpenAppWindow(url);
        }
        // No console to hide: the exe is Windows-subsystem now, and any
        // console window that DOES exist belongs to the terminal the
        // operator launched from -- hiding their terminal is not ours to
        // do. Diagnostics routing is BindStdio()'s job.
      }
    } else {
      std::printf("WARNING: %s -- running without the panel\n", ui_err.c_str());
      ui.reset();
    }
  }

  // Ops log, panel-mirrored from the start. Consecutive repeats collapse:
  // a retrying failure is one fact, not a scrolling wall (live 2026-08-29,
  // five identical invite-failure lines filled the ticker).
  std::vector<std::string> ops_log;
  const auto log_op = [&](const std::string& line) {
    if (!ops_log.empty() && ops_log.back() == line) return;
    std::printf("\n[zcomms] %s\n", line.c_str());
    ops_log.push_back(line);
    if (ops_log.size() > 5) ops_log.erase(ops_log.begin());
  };

  // Extern feeds: app-lifetime rig plumbing, restored from feeds.env now so
  // a latched feed is already flowing when the first meeting comes up.
  FeedBank feeds;
  feeds.LoadEnv(log_op);
  const auto process_feeds = [&]() {
    std::vector<std::string> cmds;
    {
      std::lock_guard<std::mutex> lock(edge_m);
      cmds.swap(feed_cmds);
    }
    for (const std::string& c : cmds) log_op(feeds.Apply(c));
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
      if (!signed_in.load()) {
        // No join card until there is an account to join as.
        publish_phase("signin", "SIGN IN WITH ZOOM TO BEGIN");
        if (quit_req.load()) return 0;
        Sleep(100);
        continue;
      }
      publish_phase("idle", "PASTE A MEETING LINK");
      if (quit_req.load()) return 0;
      process_feeds();  // feeds are configurable before any meeting exists
      Sleep(100);
      std::lock_guard<std::mutex> lock(join_m);
      if (!join_pending.empty()) {
        meeting_arg = join_pending;
        join_pending.clear();
      }
    } else {
      // Headless fallback: the console conversation. Sign-in needs the
      // panel's callback server, so headless requires an existing session
      // (or the explicit --anon escape hatch).
      if (!signed_in.load()) {
        std::printf("ERROR: not signed in with Zoom. Run the panel once to "
                    "sign in, or pass --anon for a same-account guest join.\n");
        return 2;
      }
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
  // Credentials first, SDK second: the signed-in join needs a broker-minted
  // SDK JWT + the operator's ZAK, and fetching them before InitSDK means an
  // expired sign-in never leaves a half-initialised SDK behind.
  std::string err;
  std::string sdk_jwt, zak;
  if (!cfg.anon) {
    publish_phase("joining", "CHECKING ZOOM SIGN-IN...");
    if (!oauth.EnsureJoinCredentials(&sdk_jwt, &zak, &err)) {
      log_op("Zoom sign-in problem: " + err);
      // A dead session sends the panel back to the sign-in card, not the
      // join card -- retrying the join can never fix a revoked token.
      if (!oauth.signed_in()) signed_in.store(false);
      return -1;
    }
  }

  ZoomClient zoom;
  if (!zoom.Init(&err)) {
    log_op(err + " -- " + SdkConflictHint());
    return -1;
  }
  const bool auth_ok =
      cfg.anon ? zoom.Authenticate(cfg.public_app_key, "", "", 30000, &err)
               : zoom.AuthenticateWithJwt(sdk_jwt, 30000, &err);
  if (!auth_ok) {
    log_op("SDK auth failed: " + err);
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
  const auto join_tick = [&]() -> bool {
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
    // false aborts the join: the panel's LEAVE/CANCEL is the operator's
    // exit from a stuck waiting room -- killing the app was the only way
    // out before (owner, 2026-08-30).
    return !leave_req.load() && !quit_req.load();
  };
  if (!zoom.Join(cfg.meeting_number, cfg.meeting_password, cfg.display_name,
                 600000, &err, join_tick, zak)) {
    if (leave_req.exchange(false)) {
      log_op("join cancelled");
    } else {
      log_op("could not join: " + err);
    }
    zoom.Cleanup();
    return -1;
  }
  std::printf("[zoom] in the meeting\n");
  // The stretch between "joined" and "desk ready" (channel creation, roster
  // load, invites) used to render as a blank panel -- say what is happening
  // at each stage instead (owner, 2026-08-30).
  publish_phase("joining", "IN THE MEETING -- SETTING UP");
  if (!zoom.JoinVoip(&err)) {
    std::printf("WARNING: %s\n", err.c_str());
  }
  // Talkback DELIVERY requires this client's meeting audio to be OPEN --
  // owner-found live 2026-08-29: joined muted (mute-on-entry), every send
  // was accepted and every member heard silence; unmuting made the same
  // channel audible. Open it now and keep it open (housekeeping re-opens).
  if (!zoom.UnmuteSelf(&err)) {
    std::printf("WARNING: could not open meeting audio: %s\n", err.c_str());
  }
  // ...and auto-suppress what the open mic would otherwise broadcast: the
  // ZoomISO pattern, confirmed by Zoom directly (2026-08-29, via the owner):
  // own the raw mic buffers and "mute" by sending nothing. An installed,
  // never-fed virtual mic gives an open-but-silent meeting mic, so the
  // room hears nothing while the channels stay deliverable. Under an auth
  // tier without the raw-data entitlement the callbacks never fire -- the
  // meeting then hears whatever device Zoom captures, and the operator
  // must point Zoom at a dead input; say which world we are in.
  ZoomMicSource silent_mic;  // deliberately never fed
  if (zoom.InstallVirtualMic(&silent_mic, &err)) {
    log_op("meeting mic auto-suppressed (open but silent to the room)");
  } else {
    log_op("mic auto-suppress unavailable (" + err +
           ") -- the room may hear Zoom's mic device");
  }

  Roster roster;
  roster.Attach(zoom.GetParticipantsController());

  // Breakout awareness (delivery law #2): talkback does not cross rooms,
  // so the desk tracks its own room and everyone else's, refuses what
  // cannot deliver, and can move itself.
  BreakoutRooms bo;
  bo.Attach(zoom.GetBOController());
  BreakoutState bo_state;
  std::string prev_room = "\x01";  // impossible value forces the first log
  // The operator's desired sub-production layout. Session-scoped, never
  // static -- per-meeting state dies with the meeting.
  RoomLayout desired;

  // Chat as the data side-channel: cues between desks, assignment notices
  // to talent on stock Zoom clients, fallback signaling when talkback is
  // down. The sender id is resolved to a NAME at callback time and never
  // stored (ids are meeting-scoped).
  ChatSignals chat;
  chat.Attach(zoom.GetChatController(),
              [&](const SignalMsg& m, unsigned int sender_id) {
                std::string who = "user " + std::to_string(sender_id);
                for (const RosterMember& r : roster.others()) {
                  if (r.user_id == sender_id) { who = r.name; break; }
                }
                switch (m.kind) {
                  case SignalKind::kCue:
                    log_op("cue from " + who + ": CH " +
                           std::to_string(m.slot + 1) + (m.on ? " ON" : " off"));
                    break;
                  case SignalKind::kFallback:
                    log_op(who + (m.on ? ": talkback down -- cues via chat"
                                       : ": talkback restored"));
                    break;
                  case SignalKind::kAssign:
                    log_op(who + " assigned " + m.channel_name);
                    break;
                  case SignalKind::kHello:
                    log_op(who + " is on the signaling net");
                    break;
                }
              });

  // --- Channel bring-up (host/co-host retry, as proven in the spike) --------
  const int n_channels =
      cfg.channels < 1 ? 1
                       : (cfg.channels > TalkbackChannels::kMaxChannels
                              ? TalkbackChannels::kMaxChannels
                              : cfg.channels);
  TalkbackChannels bank(zoom.GetTalkbackController());
  if (!bank.meeting_supports_talkback()) {
    log_op("this meeting does not support talkback -- cues via chat");
    // Requirement: when talkback is unavailable, chat becomes the cue
    // path -- tell every desk before leaving (one paced flush).
    chat.SignalFallback(true);
    chat.Tick(NowNs() / 1'000'000);
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
    int last_ready = -1;
    for (int i = 0; i < 100 && bank.channels_ready() < n_channels; ++i) {
      zoom.Pump(100);
      const int ready_now = bank.channels_ready();
      if (ready_now != last_ready) {
        last_ready = ready_now;
        publish_phase("joining", "IN THE MEETING -- CHANNELS " +
                                     std::to_string(ready_now) + "/" +
                                     std::to_string(n_channels));
      }
    }
    // The promote-retry wait is also cancellable -- this loop can sit for
    // ten minutes if nobody grants the role, and the operator needs an
    // exit that is not killing the app.
    if (leave_req.exchange(false) || quit_req.load()) {
      log_op("left the meeting");
      zoom.Leave();
      zoom.Cleanup();
      return quit_req.load() ? 0 : -1;
    }
    created = bank.channels_ready() >= n_channels;
    // A meeting may grant fewer than the full bank; a partial bank is a
    // working intercom, not a bring-up failure. Give the full count three
    // rounds, then take what Zoom gave.
    if (!created && attempt >= 2 && bank.channels_ready() > 0) {
      log_op(std::to_string(bank.channels_ready()) + " of " +
             std::to_string(n_channels) + " channels granted -- continuing");
      created = true;
      break;
    }
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
    chat.SignalFallback(true);
    chat.Tick(NowNs() / 1'000'000);
    zoom.Leave();
    zoom.Cleanup();
    return -1;
  }
  // Channel volume is owned by the DuckPlanner in the main loop: unity the
  // moment each channel is ready (Zoom ducks members by default, and talent
  // hears the drop on mere assignment -- CoreVideo, live 2026-08-30), duck
  // only while that channel is keyed. Late responses from a partial grant
  // are covered because the planner heals against ready_mask every tick.
  std::printf("[zoom] %d channel(s) up\n", n_channels);

  // --- Audio path -----------------------------------------------------------
  // Normal mode: the engine, mic through gain/limiter/PTT into the channel.
  // Test mode: an internally generated beep pattern through the same ring and
  // pacer, so the paced TX path is exercised identically -- only the source
  // differs.
  ChannelBankSink sink(&bank, &feeds);
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
  // Per-(slot,person) invite pacing: {attempts, earliest next try}. Zoom
  // rate-limits back-to-back talkback calls; hammering a refusal every
  // housekeeping pass turns one code-18 into a permanent wall of them.
  std::map<std::pair<int, unsigned int>, std::pair<int, int64_t>>
      invite_backoff;
  int heal_rr = 0;  // round-robin start slot for the one-call-per-pass healer
  int64_t room_move_grace_ns = 0;  // ride out a deliberate BO move
  bool room_move_departed = false; // the move has visibly left INMEETING
  int64_t next_heal_ns = 0;
  uint32_t prev_keys = 0;      // for keyed-an-empty-channel warnings
  DuckPlanner duck;            // unity at creation, duck only while keyed
  uint32_t prev_sent_mask = 0; // for first-audio-into-channel notices
  int64_t keyed_silent_since = 0;
  // Device lists for the settings drawer, refreshed on a lazy cadence --
  // WASAPI enumeration is cheap but not free, and devices rarely change.
  int64_t next_dev_ns = 0;
  std::string dev_json;
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

  while (!quit &&
         (zoom.session_alive() || NowNs() < room_move_grace_ns)) {
    // The grace window clears only after the move has VISIBLY departed
    // INMEETING and come back -- clearing on the first still-INMEETING tick
    // re-armed the meeting-over exit before the transition even began
    // (live 2026-08-30, hop attempt #3: "room move complete" logged
    // instantly, then JOIN_BREAKOUT_ROOM/RECONNECTING/CONNECTING killed
    // the loop).
    if (room_move_grace_ns != 0) {
      if (!zoom.in_meeting()) {
        room_move_departed = true;
      } else if (room_move_departed) {
        room_move_grace_ns = 0;
        room_move_departed = false;
        log_op("room move complete");
      }
    }
    if (end_ns != 0 && NowNs() >= end_ns) break;
    heartbeat_ns.store(NowNs());
    zoom.Pump(30);
    chat.Tick(NowNs() / 1'000'000);

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

      // Room truth, refreshed with the housekeeping cadence.
      bo_state = bo.Snapshot();
      if (bo_state.my_room != prev_room) {
        prev_room = bo_state.my_room;
        if (bo_state.started || !bo_state.my_room.empty()) {
          log_op("station room: " +
                 (bo_state.my_room.empty() ? "MAIN" : bo_state.my_room));
        }
      }

      // Talkback only delivers while the meeting mic is open; a host mute
      // (or mute-all) silently kills every channel, so re-open and say so.
      if (zoom.SelfMuted()) {
        std::string uerr;
        if (zoom.UnmuteSelf(&uerr)) {
          log_op("meeting mic re-opened -- talkback delivery needs it");
        } else {
          log_op("meeting mic is MUTED (talkback is dead until it opens): " +
                 uerr);
        }
      }

      // Prune intent for people no longer here: user ids are meeting-scoped
      // and recycled (plan §5) -- a stale id must not keep a channel
      // "occupied" or the direct-key pool drains on churn.
      // NEVER while breakouts run: the roster only shows this room's
      // occupants, so cross-room people "vanish" and pruning them turned
      // the healer DESTRUCTIVE (live 2026-08-30: one station hop and it
      // began removing every main-floor member from their channels --
      // only Zoom's cross-room refusal stopped it).
      if (!bo_state.started) {
        std::set<unsigned int> present;
        for (const RosterMember& m : roster.others()) present.insert(m.user_id);
        for (auto it = intent.begin(); it != intent.end();) {
          it = present.count(it->second) ? std::next(it) : intent.erase(it);
        }
        for (auto it = invite_backoff.begin(); it != invite_backoff.end();) {
          it = present.count(it->first.second) ? std::next(it)
                                               : invite_backoff.erase(it);
        }
      }

      // Default policy: a new capable participant gets their OWN channel --
      // that channel's key becomes their direct-talk key, and all-call spans
      // the bank. Past 16 people, the least-loaded channel takes the
      // spillover (a shared direct key beats being unreachable).
      const std::vector<ChannelState> asnap = bank.Snapshot();
      const int nslots = static_cast<int>(asnap.size());
      const auto slot_load = [&](int s) {
        std::set<unsigned int> ids = asnap[static_cast<size_t>(s)].members;
        for (const auto& iv : intent) {
          if (iv.first == s) ids.insert(iv.second);
        }
        return static_cast<int>(ids.size());
      };
      for (const RosterMember& m : roster.others()) {
        // Inside a breakout the SDK can list this station itself under a
        // fresh id -- auto-assigning it drew a self-invite (code 3, live
        // 2026-08-30). Our own name is never talent.
        if (m.name == cfg.display_name) continue;
        if (!m.supports_talkback) {
          if (warned_no_talkback.insert(m.user_id).second) {
            log_op(m.name + " cannot receive talkback (web client) -- skipped");
          }
          continue;
        }
        if (auto_assigned.insert(m.user_id).second) {
          int pick = -1, best = 1 << 30;
          for (int s = 0; s < nslots; ++s) {
            if (!asnap[static_cast<size_t>(s)].ready) continue;
            const int load = slot_load(s);
            if (load == 0) { pick = s; break; }
            if (load < best) { best = load; pick = s; }
          }
          if (pick >= 0) {
            intent.insert({pick, m.user_id});
            log_op(m.name + " -> CH " + std::to_string(pick + 1));
            // The courtesy chat is OPT-IN (--announce): auto-fired at
            // bring-up it messaged an entire production's audience
            // (Office Hours, 2026-08-30). The operator can still send it
            // deliberately per channel via the panel's notify verb.
            if (cfg.announce) {
              chat.SendAssignNotice(m.user_id, m.name,
                                    "CH " + std::to_string(pick + 1));
            }
          }
        }
      }

    }

    // Membership healing on its own, faster cadence: ONE call per tick,
    // rotated across channels -- Zoom's limiter is per CALL, so even
    // per-channel batches fired back-to-back in one sweep draw code 18
    // (proven live twice today: first one call per person, then one batch
    // per channel). 600ms per call sits inside the limiter (CoreVideo's
    // create ladder paces at 300ms) and fills an 8-person desk in ~5s;
    // a refused person still backs off 5s..60s.
    if (NowNs() >= next_heal_ns && bank.channels_ready() > 0) {
      next_heal_ns = NowNs() + 600'000'000LL;
      const std::vector<ChannelState> snap = bank.Snapshot();
      const int heal_n = static_cast<int>(snap.size());
      bool acted = false;
      for (int k = 0; k < heal_n && !acted; ++k) {
        const int s = (heal_rr + k) % heal_n;
        std::vector<unsigned int> missing;
        std::string missing_names;
        unsigned int to_remove = 0;
        bool have_remove = false;
        for (const RosterMember& m : roster.others()) {
          const bool want = intent.count({s, m.user_id}) != 0;
          const bool have =
              snap[static_cast<size_t>(s)].members.count(m.user_id) != 0;
          if (want && have) invite_backoff.erase({s, m.user_id});
          // A cross-room invite fails WRONG_USAGE and a cross-room member
          // hears nothing (delivery law #2) -- do not churn on it; the
          // person becomes invitable again the moment rooms align.
          const bool same_room =
              !bo_state.started ||
              BreakoutRooms::RoomOf(bo_state, m.name) == bo_state.my_room;
          if (want && !have && m.supports_talkback && same_room) {
            const auto b = invite_backoff.find({s, m.user_id});
            if (b == invite_backoff.end() || NowNs() >= b->second.second) {
              missing.push_back(m.user_id);
              if (!missing_names.empty()) missing_names += ", ";
              missing_names += m.name;
            }
          } else if (!want && have && same_room && !have_remove) {
            // Removals are room-scoped like invites (cross-room fails code
            // 3) and get the same backoff -- a repeatedly-refused remove
            // must not churn every pass (live 2026-08-30).
            const auto rb = invite_backoff.find({s, m.user_id});
            if (rb == invite_backoff.end() || NowNs() >= rb->second.second) {
              to_remove = m.user_id;
              have_remove = true;
            }
          }
        }
        if (!missing.empty()) {
          const bool ok = bank.InviteMany(s, missing, &err);
          if (!ok) {
            log_op("CH " + std::to_string(s + 1) + " invite (" + missing_names +
                   ") failed: " + err);
          }
          for (const unsigned int uid : missing) {
            auto& b = invite_backoff[{s, uid}];
            b.first = ok ? 0 : b.first + 1;
            // After a successful Execute the join confirmation is async;
            // 10s of patience before re-asking. A refusal doubles from 5s.
            const int64_t wait_ns =
                ok ? 10'000'000'000LL
                   : std::min<int64_t>(60'000'000'000LL,
                                       5'000'000'000LL << std::min(b.first, 4));
            b.second = NowNs() + wait_ns;
          }
          acted = true;
          heal_rr = s + 1;
        } else if (have_remove) {
          if (!bank.Remove(s, to_remove, &err)) {
            log_op("CH " + std::to_string(s + 1) + " remove failed: " + err);
            auto& b = invite_backoff[{s, to_remove}];
            b.first += 1;
            b.second = NowNs() + 30'000'000'000LL;
          }
          acted = true;
          heal_rr = s + 1;
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
        if (slot < 0) {  // latch-all edge from the panel
          latch_mask = on ? ((n_channels >= 32) ? 0xFFFFFFFFu
                                                : ((1u << n_channels) - 1u))
                          : 0u;
        } else if (on) {
          latch_mask |= 1u << slot;
        } else {
          latch_mask &= ~(1u << slot);
        }
      }
      for (const auto& [slot, uid, on] : aedges) {
        if (on) intent.insert({slot, uid});
        else intent.erase({slot, uid});
      }
    }

    // Chat cue/notify edges from the panel: slot -> members -> queued,
    // paced chat sends. Names resolve at send time through the roster.
    {
      std::vector<std::pair<int, bool>> cedges;
      std::vector<int> nedges;
      {
        std::lock_guard<std::mutex> lock(edge_m);
        cedges.swap(cue_edges);
        nedges.swap(notify_edges);
      }
      if (!cedges.empty() || !nedges.empty()) {
        const std::vector<ChannelState> csnap = bank.Snapshot();
        for (const auto& [slot, on] : cedges) {
          if (slot < 0 || slot >= static_cast<int>(csnap.size())) continue;
          SignalMsg sm;
          sm.kind = SignalKind::kCue;
          sm.slot = slot;
          sm.on = on;
          sm.from = cfg.display_name;
          for (const unsigned int uid : csnap[static_cast<size_t>(slot)].members) {
            chat.SendSignalTo(uid, sm);
          }
          log_op("cue " + std::string(on ? "ON" : "off") + " -> CH " +
                 std::to_string(slot + 1) + " (" +
                 std::to_string(csnap[static_cast<size_t>(slot)].members.size()) +
                 " member(s))");
        }
        for (const int slot : nedges) {
          if (slot < 0 || slot >= static_cast<int>(csnap.size())) continue;
          const std::string ch_name = "CH " + std::to_string(slot + 1);
          int sent = 0;
          for (const RosterMember& m : roster.others()) {
            if (csnap[static_cast<size_t>(slot)].members.count(m.user_id)) {
              chat.SendAssignNotice(m.user_id, m.name, ch_name);
              ++sent;
            }
          }
          log_op("assignment notice -> " + ch_name + " (" +
                 std::to_string(sent) + " member(s))");
        }
      }
    }

    // Sub-production commands (bo layout / apply / start / stop). The
    // layout is declared once; `apply` runs ONE convergence pass through
    // the planner -- creates batched into a single transaction, assigns
    // by name -- and is safe to run repeatedly (idempotent by design).
    {
      std::vector<std::string> cmds;
      {
        std::lock_guard<std::mutex> lock(edge_m);
        cmds.swap(bo_cmds);
      }
      const auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string()
                                      : s.substr(b, e - b + 1);
      };
      for (const std::string& cmd : cmds) {
        const auto sp = cmd.find(' ');
        const std::string sub =
            sp == std::string::npos ? cmd : cmd.substr(0, sp);
        const std::string rest =
            sp == std::string::npos ? "" : cmd.substr(sp + 1);
        if (sub == "layout") {
          // <room>:<person>,<person>;<room>:<person>
          desired.rooms.clear();
          size_t at = 0;
          while (at <= rest.size()) {
            size_t semi = rest.find(';', at);
            if (semi == std::string::npos) semi = rest.size();
            const std::string part = rest.substr(at, semi - at);
            const size_t colon = part.find(':');
            if (colon != std::string::npos) {
              std::vector<std::string> members;
              size_t m = colon + 1;
              while (m <= part.size()) {
                size_t comma = part.find(',', m);
                if (comma == std::string::npos) comma = part.size();
                const std::string name = trim(part.substr(m, comma - m));
                if (!name.empty()) members.push_back(name);
                m = comma + 1;
              }
              const std::string room = trim(part.substr(0, colon));
              if (!room.empty()) desired.rooms.emplace_back(room, members);
            }
            at = semi + 1;
          }
          log_op("bo: layout staged -- " +
                 std::to_string(desired.rooms.size()) +
                 " room(s); run 'bo apply'");
        } else if (sub == "apply") {
          bo_state = bo.Snapshot();
          const std::vector<RoomAction> plan = PlanRooms(desired, bo_state);
          std::vector<std::string> creates;
          bool need_start = false;
          for (const RoomAction& a : plan) {
            if (a.kind == RoomActionKind::kCreateRoom) creates.push_back(a.arg1);
            if (a.kind == RoomActionKind::kNeedStart) need_start = true;
          }
          if (!creates.empty() && !bo.CreateRooms(creates, &err)) {
            log_op("bo: create failed: " + err);
          }
          for (const RoomAction& a : plan) {
            const bool assign = a.kind == RoomActionKind::kAssignUser ||
                                a.kind == RoomActionKind::kAssignRunning ||
                                a.kind == RoomActionKind::kSwitchRunning;
            if (assign && !bo.AssignByName(a.arg1, a.arg2, bo_state.started,
                                           &err)) {
              log_op("bo: " + a.arg1 + " -> " + a.arg2 + " failed: " + err);
            }
          }
          if (need_start) log_op("bo: layout staged -- run 'bo start'");
          if (plan.empty()) log_op("bo: layout converged");
        } else if (sub == "start") {
          if (!bo.StartSession(&err)) log_op("bo: start failed: " + err);
        } else if (sub == "stop") {
          if (!bo.StopSession(&err)) log_op("bo: stop failed: " + err);
        } else {
          log_op("bo: unknown command '" + sub + "'");
        }
      }
    }

    // Room-world stale-flush (delivery law #2): ANY room transition -- the
    // station's, a member's, a start/stop -- re-provisions talkback
    // membership within one healer pass. Cross-room invites are already
    // skipped, so re-provisioning is flush-and-converge, not a second
    // membership engine.
    if (bo.ConsumeRoomsDirty()) {
      bo_state = bo.Snapshot();
      invite_backoff.clear();
      next_heal_ns = 0;  // heal NOW
    }

    // Publish the panel state. Built every tick and cheap; the server samples
    // it at its own cadence, so a slow tab costs nothing here.
    if (ui && NowNs() >= next_dev_ns) {
      next_dev_ns = NowNs() + 5'000'000'000LL;
      // mics stays a flat name list (the mic picker's contract); micchans is
      // a parallel array of native channel counts, 0 = unknown, which the
      // extern-feed channel picker reads.
      std::string dj = "\"mics\":[";
      std::string cj = "\"micchans\":[";
      bool df = true;
      for (const auto& d : ListCaptureDevices()) {
        if (!df) {
          dj += ",";
          cj += ",";
        }
        df = false;
        dj += "\"" + JsonEscape(d.name) + "\"";
        cj += std::to_string(d.channels);
      }
      dj += "]," + cj + "],\"outs\":[";
      df = true;
      for (const auto& d : ListPlaybackDevices()) {
        if (!df) dj += ",";
        df = false;
        dj += "\"" + JsonEscape(d.name) + "\"";
      }
      dj += "]";
      dev_json = dj;
    }
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
      // A channel whose whole population is one person is that person's
      // direct line; its key wears their name.
      std::map<unsigned int, std::string> names;
      for (const RosterMember& m : roster.others()) names[m.user_id] = m.name;
      const auto channel_label = [&](int s) -> std::string {
        std::set<unsigned int> ids = snap[static_cast<size_t>(s)].members;
        for (const auto& iv : intent) {
          if (iv.first == s) ids.insert(iv.second);
        }
        if (ids.size() != 1) return "";
        const auto it = names.find(*ids.begin());
        return it == names.end() ? "" : it->second;
      };
      // A person's room, relative to ours: "" when reachable (same room or
      // no breakouts), the room's name -- or "MAIN" -- when talkback
      // cannot reach them from here.
      const auto room_of = [&](const std::string& name) -> std::string {
        if (name.empty() || !bo_state.started) return "";
        const std::string r = BreakoutRooms::RoomOf(bo_state, name);
        if (r == bo_state.my_room) return "";
        return r.empty() ? "MAIN" : r;
      };
      std::string j = "{\"phase\":\"up\",";
      j += "\"meeting\":\"" + std::to_string(cfg.meeting_number) + "\",";
      j += "\"status\":\"" + status + "\",";
      j += std::string("\"talking\":") + (talking ? "true," : "false,");
      j += std::string("\"sidetone\":") + (sidetone_on ? "true," : "false,");
      j += std::string("\"aec\":") +
           ((engine && engine->aec_enabled()) ? "true," : "false,");
      j += std::string("\"tone\":") +
           ((engine && engine->test_tone()) ? "true," : "false,");
      j += std::string("\"sdkmic\":") + (zoom.SelfMuted() ? "false," : "true,");
      j += std::string("\"bostarted\":") + (bo_state.started ? "true," : "false,");
      j += "\"boroom\":\"" + JsonEscape(bo_state.my_room) + "\",";
      j += "\"rooms\":[";
      {
        bool rf = true;
        for (const BreakoutRoomInfo& r : bo_state.rooms) {
          if (!rf) j += ",";
          rf = false;
          j += "{\"id\":\"" + JsonEscape(r.id) + "\",\"name\":\"" +
               JsonEscape(r.name) + "\"}";
        }
      }
      j += "],";
      j += "\"gain\":" + std::to_string(static_cast<int>(gain_db)) + ",";
      char pk[32];
      std::snprintf(pk, sizeof(pk), "%.4f", peak);
      j += std::string("\"peak\":") + pk + ",";
      j += "\"sends\":" + std::to_string(sends) + ",";
      j += "\"underruns\":" + std::to_string(unders) + ",";
      j += "\"fails\":" + std::to_string(bank.send_failures()) + ",";
      j += "\"chsends\":" + std::to_string(bank.channel_sends()) + ",";
      j += "\"txpeak\":" + std::to_string(sink.tx_peak()) + ",";
      j += "\"mic\":\"" +
           JsonEscape(engine ? engine->capture_device_name() : "") + "\",";
      j += "\"out\":\"" +
           JsonEscape(engine ? engine->monitor_device_name() : "") + "\",";
      j += dev_json + ",";
      j += "\"feeds\":[";
      {
        bool ff = true;
        for (const FeedBank::Status& fs : feeds.Snapshot()) {
          if (!ff) j += ",";
          ff = false;
          j += "{\"slot\":" + std::to_string(fs.slot) + ",";
          j += "\"spec\":\"" + JsonEscape(fs.spec) + "\",";
          j += "\"gain\":" + std::to_string(static_cast<int>(fs.gain_db)) + ",";
          j += std::string("\"latch\":") + (fs.latch ? "true," : "false,");
          j += std::string("\"ok\":") + (fs.dev_ok ? "true," : "false,");
          j += "\"peak\":" + std::to_string(fs.peak) + "}";
        }
      }
      j += "],";
      j += "\"channels\":[";
      bool first = true;
      for (int s = 0; s < static_cast<int>(snap.size()); ++s) {
        const ChannelState& c = snap[static_cast<size_t>(s)];
        if (!first) j += ",";
        first = false;
        const std::string lbl = channel_label(s);
        j += "{\"name\":\"" + JsonEscape(c.name) + "\",";
        j += "\"label\":\"" + JsonEscape(lbl) + "\",";
        j += "\"room\":\"" + JsonEscape(room_of(lbl)) + "\",";
        // Reach truth per channel: who can hear a key RIGHT NOW, and who
        // is present-but-elsewhere (rendered dark with their room).
        {
          std::vector<std::string> member_names;
          std::set<unsigned int> mids = c.members;
          for (const auto& iv : intent) {
            if (iv.first == s) mids.insert(iv.second);
          }
          for (const unsigned int id : mids) {
            const auto it = names.find(id);
            if (it != names.end()) member_names.push_back(it->second);
          }
          const ChannelReach cr = ReachFor(bo_state, member_names);
          j += "\"reach\":{\"ok\":[";
          bool f1 = true;
          for (const std::string& n : cr.reachable) {
            if (!f1) j += ",";
            f1 = false;
            j += "\"" + JsonEscape(n) + "\"";
          }
          j += "],\"dark\":[";
          f1 = true;
          for (const auto& [n, rm] : cr.elsewhere) {
            if (!f1) j += ",";
            f1 = false;
            j += "[\"" + JsonEscape(n) + "\",\"" + JsonEscape(rm) + "\"]";
          }
          j += "]},";
        }
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
        j += "\"room\":\"" + JsonEscape(room_of(m.name)) + "\",";
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
    process_feeds();
    if (quit_req.load()) quit = true;
    if (leave_req.exchange(false)) {
      // Leave the meeting, keep the app: break to teardown; the session
      // cycle returns to the join card.
      break;
    }
    {
      const int sr = side_req.exchange(-1);
      if (sr >= 0) {
        sidetone_on = sr == 1;
        if (engine) engine->SetSidetoneEnabled(sidetone_on);
      }
      const int ar = aec_req.exchange(-1);
      if (ar >= 0 && engine) engine->SetAecEnabled(ar == 1);
      const int tr = tone_req.exchange(-1);
      if (tr >= 0 && engine) {
        engine->SetTestTone(tr == 1);
        log_op(tr == 1 ? "test tone ON -- replaces the mic in the live chain"
                       : "test tone off");
      }
      if (gain_pending.exchange(false)) {
        gain_db = static_cast<double>(gain_req.load());
        if (engine) engine->SetInputGainDb(gain_db);
      }
      // Device switches from the settings drawer: restart the engine on the
      // new device. A short gap in the mic is inherent to changing it.
      std::string want_mic, want_out, want_room;
      {
        std::lock_guard<std::mutex> lock(dev_m);
        want_mic.swap(mic_pending);
        want_out.swap(out_pending);
        want_room.swap(room_pending);
      }
      if (!want_room.empty()) {
        std::string berr;
        const bool ok = (want_room == "main") ? bo.ReturnToMain(&berr)
                                              : bo.SwitchToRoom(want_room, &berr);
        if (!ok) {
          log_op("room move failed: " + berr);
        } else {
          // A breakout move is a full disconnect/reconnect under the hood
          // (live 2026-08-30: the status ran through DISCONNECTING/ENDED,
          // twice killing the session -- a status whitelist cannot tell a
          // deliberate move from a real meeting end). WE ordered this
          // transition, so ride out ANY status for a bounded window until
          // the meeting comes back.
          room_move_grace_ns = NowNs() + 20'000'000'000LL;
          log_op("moving rooms...");
        }
      }
      if ((!want_mic.empty() || !want_out.empty()) && engine) {
        if (!want_mic.empty()) cfg.mic_device = want_mic;
        if (!want_out.empty()) cfg.monitor_device = want_out;
        const bool aec_on = engine->aec_enabled();
        engine->SetTalk(false);
        engine->Stop();
        EngineConfig ecfg;
        ecfg.capture_device = cfg.mic_device;
        ecfg.monitor_device = cfg.monitor_device;
        ecfg.monitor_enabled = sidetone_on;
        ecfg.input_gain_db = gain_db;
        ecfg.aec = aec_on;
        engine = std::make_unique<AudioEngine>(ecfg, &sink);
        if (!engine->Start(&err)) {
          log_op("audio device switch failed: " + err);
        } else {
          log_op("mic: " + engine->capture_device_name());
        }
      }
    }

    // Keying. The panel keys channels individually (digits + SPACE live in
    // the panel window, focus-scoped); ALL CALL spans the bank; latch is
    // per-channel state. The bank's key mask is the single routing truth
    // the TX path reads. There is deliberately NO global keyboard hook: a
    // GetAsyncKeyState(VK_SPACE) all-call fired while typing a space in ANY
    // app -- an open mic to the whole panel from a chat window (owner,
    // live 2026-08-29: "the space bar can't be the shortcut").
    const uint32_t all = (n_channels >= 32) ? 0xFFFFFFFFu
                                            : ((1u << n_channels) - 1u);
    const uint32_t keys =
        (latch_mask | ui_talk_mask.load() |
         (ui_allcall.load() ? all : 0u)) & all;
    for (int s = 0; s < n_channels; ++s) {
      bank.SetKey(s, ((keys >> s) & 1u) != 0);
    }
    // Keying a line whose person has not landed yet (invite in flight) is
    // the silent-failure the 15:05 live test hit: audio to nobody, no
    // feedback. Say it, once per key-down, only for channels somebody is
    // MEANT to be on -- all-call sweeping empty spares is normal.
    const uint32_t newly_keyed = keys & ~prev_keys;
    if (newly_keyed != 0) {
      const std::vector<ChannelState> ks = bank.Snapshot();
      for (int s = 0; s < n_channels && s < static_cast<int>(ks.size()); ++s) {
        if (((newly_keyed >> s) & 1u) == 0) continue;
        if (!ks[static_cast<size_t>(s)].members.empty()) {
          // Members exist -- but membership does not equal reach (law #2):
          // a channel whose whole population sits in other rooms is as
          // silent as an empty one, and must say so.
          std::vector<std::string> mnames;
          for (const RosterMember& m : roster.others()) {
            if (ks[static_cast<size_t>(s)].members.count(m.user_id)) {
              mnames.push_back(m.name);
            }
          }
          const ChannelReach cr = ReachFor(bo_state, mnames);
          if (cr.reachable.empty() && !cr.elsewhere.empty()) {
            log_op("CH " + std::to_string(s + 1) +
                   " keyed -- nobody reachable (all in other rooms)");
          }
          continue;
        }
        bool meant = false;
        for (const auto& iv : intent) {
          if (iv.first == s) { meant = true; break; }
        }
        if (meant) {
          log_op("CH " + std::to_string(s + 1) +
                 " keyed -- nobody is in it yet (invite in flight)");
        }
      }
    }
    prev_keys = keys;
    talking = keys != 0;

    // Meeting-audio duck under the talkback voice: unity when idle, kDuck
    // only while a channel ACTUALLY carries audio -- the voice gate on
    // keyed slots, the feed gate on latched ones. Latch/key state alone
    // never ducks (the ZoomISO refinement, owner 2026-09-01): a latched but
    // silent extern feed leaves members' meeting audio at unity. Healed one
    // paced SDK call at a time (the per-call rate limit applies to volume
    // calls like everything else).
    {
      const uint32_t activity =
          (sink.voice_active() ? keys : 0u) | feeds.active_mask();
      VolumeAction va;
      if (duck.Next(bank.ready_mask(), activity, NowNs() / 1'000'000, &va)) {
        if (bank.SetChannelVolume(va.slot, va.volume)) {
          duck.Confirm(va);
        } else {
          duck.Fail(NowNs() / 1'000'000);
        }
      }
    }

    // TX truth-telling, both directions: name the first moment Zoom accepts
    // audio for each channel, and call out a transmitter that is keyed but
    // shipping silence while the mic is live at capture -- the two halves
    // the 15:05 no-audio hunt could not separate.
    {
      const uint32_t now_sent = bank.sent_mask();
      const uint32_t new_sent = now_sent & ~prev_sent_mask;
      if (new_sent != 0) {
        for (int s = 0; s < n_channels; ++s) {
          if ((new_sent >> s) & 1u) {
            log_op("audio flowing into CH " + std::to_string(s + 1));
          }
        }
        prev_sent_mask = now_sent;
      }
      if (talking && engine) {
        const EngineStats es = engine->stats();
        if (es.capture_peak > 0.03 && sink.tx_peak() < 100) {
          if (keyed_silent_since == 0) {
            keyed_silent_since = NowNs();
          } else if (NowNs() - keyed_silent_since > 1'500'000'000LL) {
            log_op("keyed but transmitting SILENCE (mic live at capture) -- "
                   "envelope/AEC path suspect");
            keyed_silent_since = NowNs();
          }
        } else {
          keyed_silent_since = 0;
        }
      } else {
        keyed_silent_since = 0;
      }
    }
    // The engine's envelope opens when any channel is keyed; per-channel
    // routing happens at the sink. A channel keyed mid-speech joins at full
    // level, which is how hardware panels behave.
    if (engine) engine->SetTalk(talking);

    while (g_console_keys && _kbhit()) {
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
  // Per-monitor-v2 DPI awareness, set before ANY window exists (it cannot
  // be changed after) and in code because there is no manifest: without it
  // Windows bitmap-stretches the shell window at any scale above 100% --
  // the blurry-on-high-DPI report (owner, 2026-08-30). WebView2 under
  // per-monitor-v2 rasterizes at native DPI and keeps the panel at its
  // designed logical size via devicePixelRatio.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  zc::BindStdio();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  zc::InstallCrashTrap();
  timeBeginPeriod(1);
  // Catch here rather than letting the exception escape main: an uncaught
  // throw on this thread reaches the SEH filter as an anonymous 0xE06D7363,
  // losing the type and what() -- and the join path runs on this thread.
  int rc = 1;
  try {
    rc = zc::Run(argc, argv);
  } catch (const std::exception& e) {
    zc::Die(std::string("uncaught C++ exception -- ") + typeid(e).name() +
            ": " + e.what());
  } catch (...) {
    zc::Die("uncaught C++ exception (not derived from std::exception)");
  }
  timeEndPeriod(1);
  zc::HardExit(rc);
}
