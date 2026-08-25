#include "loopback.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#include "audio_defs.h"
#include "clock.h"
#include "miniaudio.h"

namespace zc {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// A context is needed just to enumerate. Kept local and torn down immediately:
// holding a second live context alongside a device's own has caused trouble in
// other miniaudio users and buys nothing here.
bool WithContext(const std::function<void(ma_context&)>& fn) {
  ma_context ctx;
  ma_backend backends[] = {ma_backend_wasapi};
  if (ma_context_init(backends, 1, nullptr, &ctx) != MA_SUCCESS) return false;
  fn(ctx);
  ma_context_uninit(&ctx);
  return true;
}

// Resolves a name substring to a device id. Returns false if no match.
bool FindPlaybackDevice(const std::string& match, ma_device_id* out_id,
                        std::string* out_name) {
  bool found = false;
  const std::string needle = Lower(match);
  WithContext([&](ma_context& ctx) {
    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    if (ma_context_get_devices(&ctx, &playback, &playback_count, nullptr,
                               nullptr) != MA_SUCCESS) {
      return;
    }
    for (ma_uint32 i = 0; i < playback_count; ++i) {
      const std::string name = playback[i].name;
      const bool matches = needle.empty() ? playback[i].isDefault != 0
                                          : Lower(name).find(needle) != std::string::npos;
      if (matches) {
        *out_id = playback[i].id;
        *out_name = name;
        found = true;
        return;
      }
    }
    // An empty match with no device flagged default still needs an answer.
    if (needle.empty() && playback_count > 0) {
      *out_id = playback[0].id;
      *out_name = playback[0].name;
      found = true;
    }
  });
  return found;
}

}  // namespace

std::vector<AudioDeviceInfo> ListPlaybackDevices() {
  std::vector<AudioDeviceInfo> out;
  WithContext([&](ma_context& ctx) {
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &playback, &count, nullptr, nullptr) !=
        MA_SUCCESS) {
      return;
    }
    for (ma_uint32 i = 0; i < count; ++i) {
      AudioDeviceInfo info;
      info.name = playback[i].name;
      info.is_default = playback[i].isDefault != 0;
      info.index = static_cast<int>(i);
      out.push_back(info);
    }
  });
  return out;
}

// ---------------------------------------------------------------------------
// LoopbackCapture
// ---------------------------------------------------------------------------

struct LoopbackCapture::Impl {
  ma_device device{};
  bool started = false;
  Callback cb;
  std::atomic<uint64_t> frames{0};
};

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { Stop(); }

namespace {
void LoopbackDataCallback(ma_device* device, void* /*output*/, const void* input,
                          ma_uint32 frame_count) {
  // First statement in the callback, deliberately. Every millisecond spent
  // before this read would be a millisecond of bias on every sample.
  const int64_t host_ns = NowNs();
  auto* impl = static_cast<LoopbackCapture::Impl*>(device->pUserData);
  if (impl == nullptr || input == nullptr || frame_count == 0) return;
  impl->frames.fetch_add(frame_count, std::memory_order_relaxed);
  if (impl->cb) {
    impl->cb(static_cast<const float*>(input), static_cast<int>(frame_count),
             host_ns);
  }
}
}  // namespace

bool LoopbackCapture::Start(const std::string& device_match, Callback cb,
                            std::string* error) {
  ma_device_id id{};
  std::string name;
  if (!FindPlaybackDevice(device_match, &id, &name)) {
    if (error) {
      *error = device_match.empty()
                   ? "no playback device found to tap"
                   : "no playback device matching \"" + device_match + "\"";
    }
    return false;
  }
  device_name_ = name;
  impl_->cb = std::move(cb);

  // ma_device_type_loopback captures what is being rendered to a *playback*
  // device, so the playback device's id goes in the capture slot. This looks
  // wrong and is correct.
  ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
  cfg.capture.pDeviceID = &id;
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = 1;  // miniaudio downmixes; the probe is mono
  cfg.sampleRate = kSampleRate;
  cfg.dataCallback = LoopbackDataCallback;
  cfg.pUserData = impl_.get();

  if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
    if (error) *error = "failed to open loopback on \"" + name + "\"";
    return false;
  }
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    ma_device_uninit(&impl_->device);
    if (error) *error = "failed to start loopback on \"" + name + "\"";
    return false;
  }
  impl_->started = true;
  return true;
}

void LoopbackCapture::Stop() {
  if (!impl_ || !impl_->started) return;
  ma_device_uninit(&impl_->device);
  impl_->started = false;
}

uint64_t LoopbackCapture::frames_captured() const {
  return impl_ ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

// ---------------------------------------------------------------------------
// PlaybackSink
// ---------------------------------------------------------------------------

struct PlaybackSink::Impl {
  ma_device device{};
  bool started = false;
  std::mutex m;
  std::vector<float> queue;  // rendered audio waiting for the device
  std::atomic<uint64_t> underruns{0};
};

PlaybackSink::PlaybackSink() : impl_(std::make_unique<Impl>()) {}
PlaybackSink::~PlaybackSink() { Stop(); }

namespace {
void PlaybackDataCallback(ma_device* device, void* output, const void* /*input*/,
                          ma_uint32 frame_count) {
  auto* impl = static_cast<PlaybackSink::Impl*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (impl == nullptr) {
    std::memset(out, 0, frame_count * sizeof(float));
    return;
  }
  std::lock_guard<std::mutex> lock(impl->m);
  const size_t avail = impl->queue.size();
  const size_t take = std::min<size_t>(avail, frame_count);
  if (take > 0) std::memcpy(out, impl->queue.data(), take * sizeof(float));
  if (take < frame_count) {
    std::memset(out + take, 0, (frame_count - take) * sizeof(float));
    impl->underruns.fetch_add(1, std::memory_order_relaxed);
  }
  impl->queue.erase(impl->queue.begin(),
                    impl->queue.begin() + static_cast<std::ptrdiff_t>(take));
}
}  // namespace

bool PlaybackSink::Start(const std::string& device_match, std::string* error) {
  ma_device_id id{};
  std::string name;
  if (!FindPlaybackDevice(device_match, &id, &name)) {
    if (error) *error = "no playback device matching \"" + device_match + "\"";
    return false;
  }
  device_name_ = name;

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.pDeviceID = &id;
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 1;
  cfg.sampleRate = kSampleRate;
  cfg.dataCallback = PlaybackDataCallback;
  cfg.pUserData = impl_.get();

  if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
    if (error) *error = "failed to open playback on \"" + name + "\"";
    return false;
  }
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    ma_device_uninit(&impl_->device);
    if (error) *error = "failed to start playback on \"" + name + "\"";
    return false;
  }
  impl_->started = true;
  return true;
}

void PlaybackSink::Stop() {
  if (!impl_ || !impl_->started) return;
  ma_device_uninit(&impl_->device);
  impl_->started = false;
}

bool PlaybackSink::CanSend() { return impl_ && impl_->started; }

bool PlaybackSink::Send(const int16_t* pcm, int samples) {
  if (!impl_ || !impl_->started) return false;
  std::lock_guard<std::mutex> lock(impl_->m);
  // Bound the queue. If the device is not draining, extra buffering would show
  // up as latency that belongs to the harness rather than to the path being
  // calibrated.
  const size_t max_queued = static_cast<size_t>(kFrameSamples) * 4;
  if (impl_->queue.size() > max_queued) {
    impl_->queue.erase(
        impl_->queue.begin(),
        impl_->queue.begin() +
            static_cast<std::ptrdiff_t>(impl_->queue.size() - max_queued));
  }
  for (int i = 0; i < samples; ++i) {
    impl_->queue.push_back(static_cast<float>(pcm[i]) / 32768.0f);
  }
  return true;
}

uint64_t PlaybackSink::underruns() const {
  return impl_ ? impl_->underruns.load(std::memory_order_relaxed) : 0;
}

}  // namespace zc
