#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace zc {
namespace {

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

void AssignIfEmpty(std::string* dst, const std::string& src) {
  if (dst->empty()) *dst = src;
}

}  // namespace

bool ParseMeeting(const std::string& input, uint64_t* number,
                  std::string* password) {
  // A join URL carries the passcode as ?pwd=, so pull it out rather than
  // making the operator find it separately -- a mistyped passcode fails as a
  // join timeout, which is a slow and confusing way to learn about a typo.
  const auto pwd_pos = input.find("pwd=");
  if (pwd_pos != std::string::npos && password->empty()) {
    const size_t start = pwd_pos + 4;
    size_t end = input.find_first_of("&#", start);
    if (end == std::string::npos) end = input.size();
    *password = input.substr(start, end - start);
  }

  // The meeting id is the longest run of digits, which handles bare ids,
  // "812 3456 7890", and /j/<id> URLs without three separate parsers.
  std::string best;
  std::string cur;
  const auto flush = [&]() {
    if (cur.size() > best.size()) best = cur;
    cur.clear();
  };
  for (char c : input) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      cur.push_back(c);
    } else {
      // A space inside a grouped id is part of the id, not a separator.
      if (c == ' ' && !cur.empty()) continue;
      flush();
    }
  }
  flush();

  if (best.size() < 9 || best.size() > 12) return false;
  *number = std::strtoull(best.c_str(), nullptr, 10);
  return *number != 0;
}

void PrintUsage() {
  std::printf(R"(ZComms Spike A -- TX latency harness (plan section 9)

Measures true one-way audio latency from this app's Zoom virtual mic into a
real meeting, observed at a second Zoom client running on this machine.

USAGE
  zcomms_spike_a --meeting <url|id> [options]
  zcomms_spike_a --self-test
  zcomms_spike_a --calibrate [--loopback-device <name>]
  zcomms_spike_a --list-devices

OPTIONS
  --meeting <url|id>      Meeting to join. Accepts a join URL or a raw id.
  --passcode <pw>         Meeting passcode (parsed from the URL if present).
  --duration <seconds>    Measurement length. Default 300.
  --join-timeout <secs>   How long to wait to get into the meeting. Default
                          180 -- a waiting room needs a human to click admit.
  --loopback-device <s>   Substring of the output device the second Zoom
                          client is playing to. Default: system default.
  --csv <path>            Write per-burst samples for offline inspection.
  --config <path>         Local credential file. Default: local.env beside
                          the executable, then ./local.env.
  --verbose               Per-burst output as it resolves.
  --list-devices          Show playback devices and exit.
  --check-auth            Initialise and authenticate the SDK, then stop.
                          Separates a credential problem from a meeting or
                          far-end problem before either can confuse the other.
  --self-test             Run the measurement chain against a synthetic
                          known delay. No SDK, no meeting, no audio device.
  --calibrate             Measure the local render+loopback bias.

CREDENTIALS (local.env, gitignored -- never pass these on the command line)
  public_app_key=...      General app public client id, or:
  sdk_key=...             Meeting SDK key, and
  sdk_secret=...          Meeting SDK secret (used to sign a JWT locally)
  meeting_number=...      Optional default meeting
  meeting_password=...
  display_name=...
)");
}

bool ParseConfig(int argc, char** argv, Config* out, std::string* error) {
  std::string meeting_arg;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto need = [&](const char* name) -> bool {
      if (i + 1 >= argc) {
        *error = std::string(name) + " requires a value";
        return false;
      }
      return true;
    };

    if (a == "--help" || a == "-h") {
      // Deliberately not std::exit() here. Every exit path in this process has
      // to go through main's controlled teardown -- see HardExit() in main.cpp
      // for what sdk.dll does to a normal process exit.
      out->mode = Mode::kHelp;
      return true;
    } else if (a == "--self-test") {
      out->mode = Mode::kSelfTest;
    } else if (a == "--calibrate") {
      out->mode = Mode::kCalibrate;
    } else if (a == "--list-devices") {
      out->mode = Mode::kListDevices;
    } else if (a == "--check-auth") {
      out->mode = Mode::kCheckAuth;
    } else if (a == "--verbose") {
      out->verbose = true;
    } else if (a == "--meeting") {
      if (!need("--meeting")) return false;
      meeting_arg = argv[++i];
    } else if (a == "--passcode") {
      if (!need("--passcode")) return false;
      out->meeting_password = argv[++i];
    } else if (a == "--duration") {
      if (!need("--duration")) return false;
      out->duration_s = std::atoi(argv[++i]);
    } else if (a == "--join-timeout") {
      if (!need("--join-timeout")) return false;
      out->join_timeout_s = std::atoi(argv[++i]);
    } else if (a == "--loopback-device") {
      if (!need("--loopback-device")) return false;
      out->loopback_device = argv[++i];
    } else if (a == "--csv") {
      if (!need("--csv")) return false;
      out->csv_path = argv[++i];
    } else if (a == "--config") {
      if (!need("--config")) return false;
      out->config_path = argv[++i];
    } else {
      *error = "unknown argument: " + a;
      return false;
    }
  }

  // Credential file. Beside the executable first so a staged build directory
  // is self-contained, then the working directory.
  std::vector<std::string> candidates;
  if (!out->config_path.empty()) {
    candidates.push_back(out->config_path);
  } else {
    candidates.push_back("local.env");
    candidates.push_back("../local.env");
    candidates.push_back("../../local.env");
  }

  for (const std::string& path : candidates) {
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
      if (key == "public_app_key") AssignIfEmpty(&out->public_app_key, val);
      else if (key == "sdk_key") AssignIfEmpty(&out->sdk_key, val);
      else if (key == "sdk_secret") AssignIfEmpty(&out->sdk_secret, val);
      else if (key == "meeting_password") AssignIfEmpty(&out->meeting_password, val);
      else if (key == "display_name") out->display_name = val;
      else if (key == "loopback_device") AssignIfEmpty(&out->loopback_device, val);
      else if (key == "meeting_number" && meeting_arg.empty()) meeting_arg = val;
    }
    out->config_path = path;
    break;
  }

  if (!meeting_arg.empty()) {
    if (!ParseMeeting(meeting_arg, &out->meeting_number, &out->meeting_password)) {
      *error = "could not parse a meeting id out of: " + meeting_arg;
      return false;
    }
  }

  if (out->mode == Mode::kMeasure && out->meeting_number == 0) {
    *error = "no meeting given. Pass --meeting <url|id>, or set meeting_number "
             "in local.env. Run --self-test to exercise the harness without one.";
    return false;
  }
  if (out->duration_s <= 0) {
    *error = "--duration must be positive";
    return false;
  }
  return true;
}

}  // namespace zc
