#include "signal_gate.h"

#include <cmath>

namespace zc {

SignalGate::SignalGate(double threshold_dbfs, int hang_ms, int sample_rate) {
  threshold_ = static_cast<int>(32767.0 * std::pow(10.0, threshold_dbfs / 20.0));
  if (threshold_ < 1) threshold_ = 1;
  hang_samples_ = static_cast<int64_t>(hang_ms) * sample_rate / 1000;
}

bool SignalGate::Update(const int16_t* pcm, int samples) {
  bool hot = false;
  if (pcm != nullptr) {
    for (int i = 0; i < samples; ++i) {
      const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a >= threshold_) {
        hot = true;
        break;
      }
    }
  }
  if (hot) {
    hang_left_ = hang_samples_;
  } else {
    hang_left_ -= samples;
    if (hang_left_ < 0) hang_left_ = 0;
  }
  return active();
}

}  // namespace zc
