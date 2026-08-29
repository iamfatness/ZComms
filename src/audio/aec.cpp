#include "aec.h"

#include <algorithm>
#include <cmath>

#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

namespace zc {

struct EchoCanceller::Impl {
  SpeexEchoState* echo = nullptr;
  SpeexPreprocessState* preprocess = nullptr;
  std::vector<int16_t> play_partial;   // accumulates playback to frame size
  std::vector<int16_t> mic_buf;
  std::vector<int16_t> out_buf;
};

EchoCanceller::EchoCanceller(int sample_rate, int frame_samples, int tail_ms)
    : impl_(std::make_unique<Impl>()), frame_samples_(frame_samples) {
  const int tail_samples = sample_rate * tail_ms / 1000;
  impl_->echo = speex_echo_state_init(frame_samples, tail_samples);
  int sr = sample_rate;
  speex_echo_ctl(impl_->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sr);

  // The preprocessor supplies residual echo suppression: the adaptive filter
  // removes the bulk linearly, the suppressor eats what nonlinearity (cheap
  // speakers, clipping) leaves behind. Denoise stays off -- this is a
  // production intercom and operators prefer honest room sound; the echo
  // path is the only thing being fought here.
  impl_->preprocess = speex_preprocess_state_init(frame_samples, sample_rate);
  speex_preprocess_ctl(impl_->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE,
                       impl_->echo);
  int off = 0;
  speex_preprocess_ctl(impl_->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &off);
  speex_preprocess_ctl(impl_->preprocess, SPEEX_PREPROCESS_SET_AGC, &off);

  impl_->mic_buf.resize(static_cast<size_t>(frame_samples));
  impl_->out_buf.resize(static_cast<size_t>(frame_samples));
  impl_->play_partial.reserve(static_cast<size_t>(frame_samples) * 2);
}

EchoCanceller::~EchoCanceller() {
  if (impl_->preprocess != nullptr)
    speex_preprocess_state_destroy(impl_->preprocess);
  if (impl_->echo != nullptr) speex_echo_state_destroy(impl_->echo);
}

void EchoCanceller::SetEnabled(bool on) {
  std::lock_guard<std::mutex> lock(m_);
  if (enabled_ == on) return;
  enabled_ = on;
  if (on && impl_->echo != nullptr) {
    // Stale filter taps from before the bypass describe an acoustic path
    // that may no longer exist; converging fresh beats diverging confidently.
    speex_echo_state_reset(impl_->echo);
    impl_->play_partial.clear();
  }
}

void EchoCanceller::FeedPlayback(const float* samples, int count) {
  std::lock_guard<std::mutex> lock(m_);
  if (!enabled_ || impl_->echo == nullptr) return;
  for (int i = 0; i < count; ++i) {
    const float v = std::max(-1.0f, std::min(1.0f, samples[i]));
    impl_->play_partial.push_back(
        static_cast<int16_t>(std::lrintf(v * 32767.0f)));
    if (impl_->play_partial.size() == static_cast<size_t>(frame_samples_)) {
      speex_echo_playback(impl_->echo, impl_->play_partial.data());
      impl_->play_partial.clear();
    }
  }
}

void EchoCanceller::ProcessCapture(float* frame) {
  std::lock_guard<std::mutex> lock(m_);
  if (!enabled_ || impl_->echo == nullptr) return;
  for (int i = 0; i < frame_samples_; ++i) {
    const float v = std::max(-1.0f, std::min(1.0f, frame[i]));
    impl_->mic_buf[static_cast<size_t>(i)] =
        static_cast<int16_t>(std::lrintf(v * 32767.0f));
  }
  speex_echo_capture(impl_->echo, impl_->mic_buf.data(),
                     impl_->out_buf.data());
  speex_preprocess_run(impl_->preprocess, impl_->out_buf.data());
  for (int i = 0; i < frame_samples_; ++i) {
    frame[i] = static_cast<float>(impl_->out_buf[static_cast<size_t>(i)]) /
               32768.0f;
  }
}

}  // namespace zc
