// Command line and local credential loading.
//
// Credentials never appear in a committed file and never appear in a command
// line that would land in a shell history. They come from a gitignored local
// file, and everything that prints them is redacted. The Marketplace identity
// this spike borrows is not ZComms' own yet (plan §3.6), which makes keeping
// it out of the repo more important rather than less.
#pragma once

#include <string>
#include <cstdint>

namespace zc {

enum class Mode {
  kMeasure,     // the real thing: join a meeting and measure
  kSelfTest,    // prove the instrument against a known synthetic delay
  kCalibrate,   // measure the local render+loopback bias
  kListDevices,
  kCheckAuth,   // init + authenticate only, no meeting
  kHelp,
};

struct Config {
  Mode mode = Mode::kMeasure;

  // Auth. Exactly one of these is used; public_app_key takes precedence
  // because it is what a General app issues, and JWT is the SDK-key path.
  std::string public_app_key;
  std::string sdk_key;
  std::string sdk_secret;

  uint64_t meeting_number = 0;
  std::string meeting_password;
  std::string display_name = "ZComms Spike A";

  std::string loopback_device;  // substring match; empty = default output
  int duration_s = 300;
  // Generous by default. A waiting room turns joining into a human action --
  // the host has to notice the request and click admit -- and timing out
  // underneath that reads as a harness failure when it is just impatience.
  int join_timeout_s = 180;
  std::string csv_path;
  bool verbose = false;

  std::string config_path;
};

// Parses argv, then fills anything still unset from the local config file.
// CLI wins over the file so a one-off run does not need the file edited.
bool ParseConfig(int argc, char** argv, Config* out, std::string* error);

// Accepts a bare id ("81234567890"), a spaced id, or a full join URL.
bool ParseMeeting(const std::string& input, uint64_t* number,
                  std::string* password);

void PrintUsage();

}  // namespace zc
