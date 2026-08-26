#include "wav_sink.h"

#include <cstring>

#include "audio_defs.h"

namespace zc {
namespace {

void WriteU32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void WriteU16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

}  // namespace

WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(const std::string& path, int sample_rate, int channels,
                     std::string* error) {
  Close();
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    if (error) *error = "could not open " + path + " for writing";
    return false;
  }
  sample_rate_ = sample_rate;
  channels_ = channels;
  samples_ = 0;

  // Header with placeholder sizes; patched in Close(). Writing it up front
  // means a file left behind by a crashed run is still openable by most tools
  // rather than being zero bytes.
  const uint16_t bits = 16;
  const uint32_t byte_rate =
      static_cast<uint32_t>(sample_rate * channels * bits / 8);
  const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);

  std::fwrite("RIFF", 1, 4, file_);
  WriteU32(file_, 0);  // patched
  std::fwrite("WAVE", 1, 4, file_);
  std::fwrite("fmt ", 1, 4, file_);
  WriteU32(file_, 16);
  WriteU16(file_, 1);  // PCM
  WriteU16(file_, static_cast<uint16_t>(channels));
  WriteU32(file_, static_cast<uint32_t>(sample_rate));
  WriteU32(file_, byte_rate);
  WriteU16(file_, block_align);
  WriteU16(file_, bits);
  std::fwrite("data", 1, 4, file_);
  WriteU32(file_, 0);  // patched
  return true;
}

bool WavWriter::Write(const int16_t* pcm, int samples) {
  if (file_ == nullptr || pcm == nullptr || samples <= 0) return false;
  const size_t n = std::fwrite(pcm, sizeof(int16_t),
                               static_cast<size_t>(samples), file_);
  samples_ += n;
  return n == static_cast<size_t>(samples);
}

void WavWriter::Close() {
  if (file_ == nullptr) return;
  const uint32_t data_bytes = static_cast<uint32_t>(samples_ * sizeof(int16_t));
  std::fseek(file_, 4, SEEK_SET);
  WriteU32(file_, 36 + data_bytes);
  std::fseek(file_, 40, SEEK_SET);
  WriteU32(file_, data_bytes);
  std::fclose(file_);
  file_ = nullptr;
}

bool WavSink::Open(const std::string& path, std::string* error) {
  return writer_.Open(path, kSampleRate, 1, error);
}

void WavSink::Close() { writer_.Close(); }

bool WavSink::CanSend() { return writer_.open(); }

bool WavSink::Send(const int16_t* pcm, int samples) {
  return writer_.Write(pcm, samples);
}

}  // namespace zc
