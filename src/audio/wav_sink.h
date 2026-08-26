// A FrameSink that writes what would have gone to Zoom, as a WAV file.
//
// This exists so the audio engine can be exercised end to end without a
// meeting, without credentials, and without the SDK. Everything upstream of
// the sink -- capture, gain, limiting, PTT ramps, the 20 ms pacing, the ring
// and its underrun behaviour -- runs exactly as it does in production, and the
// result is a file you can look at.
//
// That matters more than it sounds. The properties this engine has to get
// right are mostly properties of a waveform: does the PTT release actually
// ramp or does it step, does the limiter hold its ceiling, does a starved ring
// insert silence or stall. Those are answerable by inspecting samples and
// essentially unanswerable by watching a meeting.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "tx_pacer.h"

namespace zc {

class WavWriter {
 public:
  WavWriter() = default;
  ~WavWriter();

  bool Open(const std::string& path, int sample_rate, int channels,
            std::string* error);
  bool Write(const int16_t* pcm, int samples);
  void Close();

  bool open() const { return file_ != nullptr; }
  uint64_t samples_written() const { return samples_; }

 private:
  std::FILE* file_ = nullptr;
  uint64_t samples_ = 0;
  int sample_rate_ = 0;
  int channels_ = 0;
};

class WavSink : public FrameSink {
 public:
  bool Open(const std::string& path, std::string* error);
  void Close();

  // Always open once the file is. The gating a real virtual mic does is Zoom's
  // to impose; a file has no such window, and pretending otherwise would mean
  // the engine behaved differently under test than in production.
  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;

  uint64_t samples_written() const { return writer_.samples_written(); }

 private:
  WavWriter writer_;
};

}  // namespace zc
