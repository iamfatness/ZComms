#include "engine.h"

#include <algorithm>
#include <cmath>

#include "audio_defs.h"

namespace zc {
namespace {
// Sidetone buffer. Deliberately small: this path crosses two device clocks and
// any slack in it is delay the operator hears on their own voice, which is the
// one artefact that actively interferes with speaking. ~60 ms is enough to
// absorb ordinary block-size mismatch and short enough to stay comfortable.
constexpr double kSidetoneBufferMs = 60.0;
}  // namespace

AudioEngine::AudioEngine(const EngineConfig& cfg, FrameSink* sink)
    : cfg_(cfg),
      sink_(sink),
      ring_(static_cast<size_t>(std::max(4, cfg.ring_frames))),
      gain_(cfg.input_gain_db, 15.0, kSampleRate),
      limiter_(cfg.limiter_ceiling_dbfs, cfg.limiter_lookahead_ms,
               cfg.limiter_release_ms, kSampleRate),
      ptt_(cfg.ptt_fade_ms, kSampleRate),
      sidetone_gain_(cfg.sidetone_db, 15.0, kSampleRate),
      sidetone_(static_cast<size_t>(kSidetoneBufferMs * kSampleRate / 1000.0)) {
  sidetone_on_.store(cfg.sidetone_enabled);
  scratch_.reserve(4096);
}

AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(std::string* error) {
  if (cfg_.aec) {
    aec_ = std::make_unique<EchoCanceller>(kSampleRate, kFrameSamples,
                                           cfg_.aec_tail_ms);
  }
  if (!capture_.Start(cfg_.capture_device,
                      [this](const float* mono, int frames) {
                        OnCapture(mono, frames);
                      },
                      error)) {
    return false;
  }

  if (cfg_.monitor_enabled) {
    if (!monitor_.Start(cfg_.monitor_device,
                        [this](float* out, int frames) { OnMonitor(out, frames); },
                        error)) {
      capture_.Stop();
      return false;
    }
  }

  pacer_ = std::make_unique<TxPacer>(&ring_, sink_, nullptr);
  pacer_->Start(cfg_.prime_frames, 500);
  return true;
}

void AudioEngine::Stop() {
  // Pacer first: it is the only thing calling into the sink, and stopping it
  // before the devices means no frame can be handed over while capture is
  // being torn down.
  if (pacer_) {
    pacer_->Stop();
    final_pacer_ = pacer_->stats();  // before the pacer goes away
    pacer_.reset();
  }
  monitor_.Stop();
  capture_.Stop();
}

void AudioEngine::SetTalk(bool on) {
  talk_.store(on);
  // The envelope is only advanced on the capture thread, so this just moves
  // the target. A press and release closer together than the fade still
  // produces a continuous ramp rather than a jump -- see envelope.h.
  if (on) {
    ptt_.Open();
  } else {
    ptt_.Close();
  }
}

void AudioEngine::SetInputGainDb(double db) { gain_.set_db(db); }
void AudioEngine::SetSidetoneDb(double db) { sidetone_gain_.set_db(db); }
void AudioEngine::SetSidetoneEnabled(bool on) { sidetone_on_.store(on); }

void AudioEngine::SetAecEnabled(bool on) {
  if (aec_) aec_->SetEnabled(on);
}

bool AudioEngine::aec_enabled() const { return aec_ && aec_->enabled(); }

const std::string& AudioEngine::capture_device_name() const {
  return capture_.device_name();
}
const std::string& AudioEngine::monitor_device_name() const {
  return monitor_.device_name();
}

void AudioEngine::OnCapture(const float* mono, int frames) {
  if (frames <= 0) return;
  // Raw capture is chunked to exact 20 ms frames FIRST and everything runs
  // per-frame: the echo canceller demands fixed frames, and every other
  // stage is indifferent to block size, so the frame is the one shape.
  accum_.Push(mono, frames, [this](const float* frame, uint64_t start_sample) {
    if (scratch_.size() < static_cast<size_t>(kFrameSamples)) {
      scratch_.resize(static_cast<size_t>(kFrameSamples));
    }
    float* buf = scratch_.data();
    std::copy(frame, frame + kFrameSamples, buf);

    // Echo cancellation before anything else touches the signal. The
    // canceller models microphone-input against monitor-output; gain or
    // limiting applied first would look like a time-varying acoustic path
    // it has to keep chasing.
    if (aec_) aec_->ProcessCapture(buf);

    gain_.Process(buf, kFrameSamples);
    limiter_.Process(buf, kFrameSamples);

    // Peak before the PTT envelope, so a level meter still moves when the
    // operator is not talking. A meter that only works while transmitting
    // is useless for setting gain.
    double peak = capture_peak_.load();
    for (int i = 0; i < kFrameSamples; ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(buf[i])));
    }
    peak *= 0.995;  // slow decay so the display falls back, not sticks
    capture_peak_.store(peak);

    ptt_.Process(buf, kFrameSamples);

    // Sidetone is tapped here, after the envelope, so it is a confidence
    // monitor of what is actually leaving rather than of what the
    // microphone heard -- and only when something is consuming it (see the
    // sidetone drop-counter note in git history).
    if (cfg_.monitor_enabled && sidetone_on_.load()) {
      sidetone_.Write(buf, static_cast<size_t>(kFrameSamples));
    }

    TxFrame f;
    f.seq = start_sample / static_cast<uint64_t>(kFrameSamples);
    for (int i = 0; i < kFrameSamples; ++i) {
      const float v = std::max(-1.0f, std::min(1.0f, buf[i]));
      f.pcm[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
    }
    ring_.Push(f);
    frames_to_ring_.fetch_add(1, std::memory_order_relaxed);
  });
}

void AudioEngine::OnMonitor(float* out, int frames) {
  if (frames <= 0) return;
  sidetone_.Read(out, static_cast<size_t>(frames));
  sidetone_gain_.Process(out, frames);
  // The far-end reference: exactly what is about to leave the speakers,
  // post-fader -- the canceller must model the path from what was actually
  // played, not a pre-gain copy.
  if (aec_) aec_->FeedPlayback(out, frames);
}

EngineStats AudioEngine::stats() const {
  EngineStats s;
  s.capture_frames = capture_.frames();
  s.monitor_frames = monitor_.frames();
  s.frames_to_ring = frames_to_ring_.load();
  s.ring_drops = ring_.drops();
  s.sidetone_drops = sidetone_.dropped();
  s.sidetone_starved = sidetone_.starved();
  s.limiter_engaged_samples = limiter_.engaged_samples();
  s.capture_peak = capture_peak_.load();
  s.pacer = pacer_ ? pacer_->stats() : final_pacer_;
  return s;
}

}  // namespace zc
