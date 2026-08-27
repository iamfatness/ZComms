#include "devices.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>

#include "audio_defs.h"
#include "miniaudio.h"

namespace zc {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool WithContext(const std::function<void(ma_context&)>& fn) {
  ma_context ctx;
  if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return false;
  fn(ctx);
  ma_context_uninit(&ctx);
  return true;
}

std::vector<DeviceInfo> ListDevices(bool capture) {
  std::vector<DeviceInfo> out;
  WithContext([&](ma_context& ctx) {
    ma_device_info* playback = nullptr;
    ma_device_info* cap = nullptr;
    ma_uint32 playback_count = 0, cap_count = 0;
    if (ma_context_get_devices(&ctx, &playback, &playback_count, &cap,
                               &cap_count) != MA_SUCCESS) {
      return;
    }
    ma_device_info* list = capture ? cap : playback;
    const ma_uint32 count = capture ? cap_count : playback_count;
    for (ma_uint32 i = 0; i < count; ++i) {
      DeviceInfo d;
      d.name = list[i].name;
      d.is_default = list[i].isDefault != 0;
      d.index = static_cast<int>(i);
      out.push_back(d);
    }
  });
  return out;
}

bool FindDevice(bool capture, const std::string& match, ma_device_id* out_id,
                std::string* out_name) {
  bool found = false;
  const std::string needle = Lower(match);
  WithContext([&](ma_context& ctx) {
    ma_device_info* playback = nullptr;
    ma_device_info* cap = nullptr;
    ma_uint32 playback_count = 0, cap_count = 0;
    if (ma_context_get_devices(&ctx, &playback, &playback_count, &cap,
                               &cap_count) != MA_SUCCESS) {
      return;
    }
    ma_device_info* list = capture ? cap : playback;
    const ma_uint32 count = capture ? cap_count : playback_count;
    for (ma_uint32 i = 0; i < count; ++i) {
      const std::string name = list[i].name;
      const bool matches = needle.empty()
                               ? list[i].isDefault != 0
                               : Lower(name).find(needle) != std::string::npos;
      if (matches) {
        *out_id = list[i].id;
        *out_name = name;
        found = true;
        return;
      }
    }
    if (needle.empty() && count > 0) {
      *out_id = list[0].id;
      *out_name = list[0].name;
      found = true;
    }
  });
  return found;
}

}  // namespace

std::vector<DeviceInfo> ListCaptureDevices() { return ListDevices(true); }
std::vector<DeviceInfo> ListPlaybackDevices() { return ListDevices(false); }

// --- CaptureDevice ----------------------------------------------------------

struct CaptureDevice::Impl {
  ma_device device{};
  bool started = false;
  Callback cb;
  std::atomic<uint64_t> frames{0};
};

CaptureDevice::CaptureDevice() : impl_(std::make_unique<Impl>()) {}
CaptureDevice::~CaptureDevice() { Stop(); }

namespace {
void CaptureCallback(ma_device* device, void* /*output*/, const void* input,
                     ma_uint32 frame_count) {
  auto* impl = static_cast<CaptureDevice::Impl*>(device->pUserData);
  if (impl == nullptr || input == nullptr || frame_count == 0) return;
  impl->frames.fetch_add(frame_count, std::memory_order_relaxed);
  if (impl->cb) impl->cb(static_cast<const float*>(input),
                         static_cast<int>(frame_count));
}
}  // namespace

bool CaptureDevice::Start(const std::string& match, Callback cb,
                          std::string* error) {
  ma_device_id id{};
  std::string name;
  if (!FindDevice(true, match, &id, &name)) {
    if (error) {
      *error = match.empty() ? "no capture device found"
                             : "no capture device matching \"" + match + "\"";
    }
    return false;
  }
  device_name_ = name;
  impl_->cb = std::move(cb);

  ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
  cfg.capture.pDeviceID = &id;
  cfg.capture.format = ma_format_f32;
  cfg.capture.channels = 1;
  // shareMode stays shared: exclusive mode would lower latency but takes the
  // device away from everything else on the machine, which for an operator
  // running a meeting client alongside is the wrong trade.
  cfg.capture.shareMode = ma_share_mode_shared;
  cfg.sampleRate = kSampleRate;
  cfg.dataCallback = CaptureCallback;
  cfg.pUserData = impl_.get();

  if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
    if (error) *error = "failed to open capture device \"" + name + "\"";
    return false;
  }
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    ma_device_uninit(&impl_->device);
    if (error) *error = "failed to start capture device \"" + name + "\"";
    return false;
  }
  impl_->started = true;
  return true;
}

void CaptureDevice::Stop() {
  if (!impl_ || !impl_->started) return;
  ma_device_uninit(&impl_->device);
  impl_->started = false;
}

bool CaptureDevice::running() const { return impl_ && impl_->started; }

uint64_t CaptureDevice::frames() const {
  return impl_ ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

// --- MonitorDevice ----------------------------------------------------------

struct MonitorDevice::Impl {
  ma_device device{};
  bool started = false;
  Callback cb;
  std::atomic<uint64_t> frames{0};
};

MonitorDevice::MonitorDevice() : impl_(std::make_unique<Impl>()) {}
MonitorDevice::~MonitorDevice() { Stop(); }

namespace {
void MonitorCallback(ma_device* device, void* output, const void* /*input*/,
                     ma_uint32 frame_count) {
  auto* impl = static_cast<MonitorDevice::Impl*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (impl == nullptr) {
    std::memset(out, 0, frame_count * sizeof(float));
    return;
  }
  impl->frames.fetch_add(frame_count, std::memory_order_relaxed);
  if (impl->cb) {
    impl->cb(out, static_cast<int>(frame_count));
  } else {
    std::memset(out, 0, frame_count * sizeof(float));
  }
}
}  // namespace

bool MonitorDevice::Start(const std::string& match, Callback cb,
                          std::string* error) {
  ma_device_id id{};
  std::string name;
  if (!FindDevice(false, match, &id, &name)) {
    if (error) {
      *error = match.empty() ? "no playback device found"
                             : "no playback device matching \"" + match + "\"";
    }
    return false;
  }
  device_name_ = name;
  impl_->cb = std::move(cb);

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.pDeviceID = &id;
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 1;
  cfg.sampleRate = kSampleRate;
  cfg.dataCallback = MonitorCallback;
  cfg.pUserData = impl_.get();

  if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
    if (error) *error = "failed to open playback device \"" + name + "\"";
    return false;
  }
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    ma_device_uninit(&impl_->device);
    if (error) *error = "failed to start playback device \"" + name + "\"";
    return false;
  }
  impl_->started = true;
  return true;
}

void MonitorDevice::Stop() {
  if (!impl_ || !impl_->started) return;
  ma_device_uninit(&impl_->device);
  impl_->started = false;
}

bool MonitorDevice::running() const { return impl_ && impl_->started; }

uint64_t MonitorDevice::frames() const {
  return impl_ ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

}  // namespace zc
