// Local audio I/O: WASAPI loopback capture, and playback for calibration.
//
// Loopback is the measurement's observation point. The second Zoom client runs
// on this machine and renders the meeting audio to an output device; tapping
// that device's render stream gives us the far end's audio inside our own
// process, on our own clock. That is what makes a single-clock measurement
// possible at all -- the alternative, timestamping at the far end, would mean
// two clocks and the brief rules it out.
//
// The tap sits at the OS mixer, so it observes audio slightly before it would
// reach a physical output. What that includes and excludes is spelled out in
// probe.h and bracketed by --calibrate.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "tx_pacer.h"

namespace zc {

// Device enumeration lives in devices.h (DeviceInfo / ListPlaybackDevices).

// Captures the render stream of a playback device, converted to 48 kHz mono
// float regardless of the device's native format.
class LoopbackCapture {
 public:
  // host_ns is read at the very top of the audio callback, before any other
  // work, so that nothing this class does can bias the timestamp.
  using Callback = std::function<void(const float* mono, int frames, int64_t host_ns)>;

  LoopbackCapture();
  ~LoopbackCapture();

  // `device_match` is a case-insensitive substring of the device name; empty
  // selects the default output. Naming the device explicitly matters because
  // tapping the wrong endpoint yields a silent capture, which is a confusing
  // failure unless it is easy to be sure which device was chosen.
  bool Start(const std::string& device_match, Callback cb, std::string* error);
  void Stop();

  const std::string& device_name() const { return device_name_; }
  uint64_t frames_captured() const;

  // Public only so the C-style audio callback can reach it; miniaudio hands
  // the callback a void* and there is no member-function form.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::string device_name_;
};

// A FrameSink that renders to a local output device. Used only by
// --calibrate, where it stands in for Zoom so the same paced TX path can
// measure the local render-plus-loopback delay on its own.
class PlaybackSink : public FrameSink {
 public:
  PlaybackSink();
  ~PlaybackSink() override;

  bool Start(const std::string& device_match, std::string* error);
  void Stop();

  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;

  const std::string& device_name() const { return device_name_; }
  uint64_t underruns() const;

  struct Impl;  // see LoopbackCapture::Impl

 private:
  std::unique_ptr<Impl> impl_;
  std::string device_name_;
};

}  // namespace zc
