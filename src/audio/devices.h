// Local audio I/O.
//
// Plan §6.3 picks miniaudio over Qt Multimedia, and the reason is the product
// thesis rather than taste: Qt's audio path adds latency on top of Zoom's, and
// latency is the thing being sold. A single header with a direct WASAPI/
// CoreAudio backend also keeps the device layer somewhere we can reason about
// when an operator's interface behaves oddly, which is a support cost that
// arrives with every real deployment.
//
// Both device types convert to 48 kHz mono float regardless of what the
// hardware natively offers, so nothing above this layer ever has to care.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zc {

struct DeviceInfo {
  std::string name;
  bool is_default = false;
  int index = 0;
};

std::vector<DeviceInfo> ListCaptureDevices();
std::vector<DeviceInfo> ListPlaybackDevices();

// Microphone / line input.
class CaptureDevice {
 public:
  using Callback = std::function<void(const float* mono, int frames)>;

  CaptureDevice();
  ~CaptureDevice();

  // `match` is a case-insensitive substring of the device name; empty selects
  // the system default.
  bool Start(const std::string& match, Callback cb, std::string* error);
  void Stop();
  bool running() const;

  const std::string& device_name() const { return device_name_; }
  uint64_t frames() const;

  struct Impl;  // public for the C-style audio callback

 private:
  std::unique_ptr<Impl> impl_;
  std::string device_name_;
};

// Multichannel line input for extern feeds: opens the device at its NATIVE
// channel count and hands over interleaved 48 kHz float frames. The channel
// pick and downmix happen above this layer (extern_feed.h), where they are
// pure -- this class exists because CaptureDevice's channels=1 open lets
// miniaudio downmix the WHOLE device, which destroys per-channel routing.
class MultiCaptureDevice {
 public:
  using Callback =
      std::function<void(const float* interleaved, int frames, int channels)>;

  MultiCaptureDevice();
  ~MultiCaptureDevice();

  bool Start(const std::string& match, Callback cb, std::string* error);
  void Stop();
  bool running() const;

  const std::string& device_name() const { return device_name_; }
  int channels() const;
  uint64_t frames() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::string device_name_;
};

// Headphone / monitor output. Pull-based: the callback is asked to fill the
// buffer, which is the shape a mixer wants.
class MonitorDevice {
 public:
  using Callback = std::function<void(float* out, int frames)>;

  MonitorDevice();
  ~MonitorDevice();

  bool Start(const std::string& match, Callback cb, std::string* error);
  void Stop();
  bool running() const;

  const std::string& device_name() const { return device_name_; }
  uint64_t frames() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::string device_name_;
};

}  // namespace zc
