#include "mic_source.h"

#include <cstdio>

#include "audio_defs.h"

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {

void ZoomMicSource::onMicInitialize(IZoomSDKAudioRawDataSender* sender) {
  {
    std::lock_guard<std::mutex> lock(sender_m_);
    sender_ = sender;
  }
  initialised_.store(true);
  std::printf("[mic] onMicInitialize -- sender acquired\n");
}

void ZoomMicSource::onMicStartSend() {
  can_send_.store(true);
  std::printf("[mic] onMicStartSend -- send window OPEN\n");
}

void ZoomMicSource::onMicStopSend() {
  can_send_.store(false);
  std::printf("[mic] onMicStopSend -- send window CLOSED\n");
}

void ZoomMicSource::onMicUninitialized() {
  // Order matters. Shut the gate before dropping the pointer, so a TX thread
  // that has already passed CanSend() cannot reach a null sender_ -- and take
  // the same lock Send() takes, so one that is already inside finishes first.
  can_send_.store(false);
  initialised_.store(false);
  {
    std::lock_guard<std::mutex> lock(sender_m_);
    sender_ = nullptr;
  }
  std::printf("[mic] onMicUninitialized -- sender revoked\n");
}

bool ZoomMicSource::CanSend() {
  return can_send_.load() && initialised_.load();
}

bool ZoomMicSource::Send(const int16_t* pcm, int samples) {
  std::lock_guard<std::mutex> lock(sender_m_);
  // Re-checked under the lock. CanSend() was a hint taken by the pacer some
  // instant earlier, and onMicStopSend can land in between.
  if (sender_ == nullptr || !can_send_.load()) return false;

  // send() takes char* and a byte length that must be even -- 16-bit PCM, so
  // it always is, but the cast is where that contract is visible.
  const SDKError err = sender_->send(
      reinterpret_cast<char*>(const_cast<int16_t*>(pcm)),
      static_cast<unsigned int>(samples * static_cast<int>(sizeof(int16_t))),
      kSampleRate, ZoomSDKAudioChannel_Mono);

  if (err != SDKERR_SUCCESS) {
    send_failures_.fetch_add(1);
    last_error_.store(static_cast<int>(err));
    return false;
  }
  return true;
}

}  // namespace zc
