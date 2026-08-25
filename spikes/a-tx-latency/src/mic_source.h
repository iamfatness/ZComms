// The virtual mic. Seeds ZoomMicSource in plan §6.1.
//
// IZoomSDKVirtualAudioMicEvent is a four-callback lifecycle and the rules are
// not advisory:
//
//   onMicInitialize(pSender)  -- the sender pointer arrives; hold it
//   onMicStartSend()          -- send() becomes legal
//   onMicStopSend()           -- send() becomes illegal again
//   onMicUninitialized()      -- the pointer is revoked; drop it
//
// So every send is gated on a flag that only those callbacks move, and the
// pointer is read under a lock that onMicUninitialized also takes. The failure
// this prevents is calling into a revoked pointer from the TX thread, which is
// a crash rather than a glitch, and which a 20 ms cadence would find quickly.
//
// This class is a FrameSink, which is what lets the same TxPacer drive Zoom,
// a local output device, or a synthetic sink without knowing the difference.
#pragma once

// windows.h must precede the SDK headers: zoom_sdk_def.h uses HWND, RECT and
// UINT64 bare, without including anything that declares them.
// clang-format off
#include <windows.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <mutex>

#include "rawdata/rawdata_audio_helper_interface.h"
#include "tx_pacer.h"
#include "zoom_sdk.h"

namespace zc {

class ZoomMicSource : public ZOOM_SDK_NAMESPACE::IZoomSDKVirtualAudioMicEvent,
                      public FrameSink {
 public:
  // IZoomSDKVirtualAudioMicEvent
  void onMicInitialize(ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataSender* sender) override;
  void onMicStartSend() override;
  void onMicStopSend() override;
  void onMicUninitialized() override;

  // FrameSink
  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;

  bool initialised() const { return initialised_.load(); }
  bool sending() const { return can_send_.load(); }
  uint64_t send_failures() const { return send_failures_.load(); }
  int last_error() const { return last_error_.load(); }

 private:
  mutable std::mutex sender_m_;
  ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataSender* sender_ = nullptr;
  std::atomic<bool> can_send_{false};
  std::atomic<bool> initialised_{false};
  std::atomic<uint64_t> send_failures_{0};
  std::atomic<int> last_error_{0};
};

}  // namespace zc
