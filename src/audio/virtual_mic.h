// Zoom's virtual microphone, as the engine needs it.
//
// Two jobs used to live in one class: implementing Zoom's
// IZoomSDKVirtualAudioMicEvent, and being the FrameSink the TX pacer drives.
// Only the second is portable, so only the second is up here. The four-
// callback lifecycle (onMicInitialize/StartSend/StopSend/Uninitialized) and
// the sender pointer belong to whichever backend the build selected.
//
// The status accessors exist because "the send window never opened" is the
// single most common talkback failure and the operator needs the reason
// attached: initialised() distinguishes "Zoom never handed us a sender"
// (usually a missing raw-data entitlement) from sending() false, which is
// "we have a sender but the window is shut" (usually a muted meeting mic).
#pragma once

#include <cstdint>

#include "tx_pacer.h"

namespace zc {

class VirtualMic : public FrameSink {
 public:
  ~VirtualMic() override = default;

  // Zoom handed us a sender (onMicInitialize fired, onMicUninitialized has
  // not). Says nothing about whether sending is legal right now.
  virtual bool initialised() const = 0;
  // The send window is OPEN -- between onMicStartSend and onMicStopSend.
  virtual bool sending() const = 0;
  virtual uint64_t send_failures() const = 0;
  // The platform SDK's own last error code, for humans. Never branched on.
  virtual int last_error() const = 0;
};

}  // namespace zc
