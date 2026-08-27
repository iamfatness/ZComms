// Shared audio constants for the Spike A harness.
//
// 48 kHz mono 16-bit is what IZoomSDKAudioRawDataSender::send() takes and what
// plan §3.2 fixes for the channel workers, so the harness uses it end to end and
// never resamples. 20 ms is the TX cadence from plan §6.1 -- it is the pacing
// period, not a buffer size, and nothing here may derive it from a device.
#pragma once

#include <cstdint>

namespace zc {

constexpr int kSampleRate = 48000;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  // 960
constexpr int kFrameBytes = kFrameSamples * static_cast<int>(sizeof(int16_t));

}  // namespace zc
