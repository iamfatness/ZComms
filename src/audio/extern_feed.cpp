#include "extern_feed.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace zc {

namespace {

// Device names contain arbitrary punctuation ("Dante Virtual Soundcard:
// RX 3-4"), so structured fields always split from the END of the string.
bool ParseChannels(const std::string& s, int* a, int* b) {
  if (s.empty()) return false;
  const size_t dash = s.find('-');
  char* end = nullptr;
  const std::string first = dash == std::string::npos ? s : s.substr(0, dash);
  *a = static_cast<int>(std::strtol(first.c_str(), &end, 10));
  if (end == nullptr || *end != '\0' || *a < 1) return false;
  if (dash == std::string::npos) {
    *b = -1;
    return true;
  }
  const std::string second = s.substr(dash + 1);
  *b = static_cast<int>(std::strtol(second.c_str(), &end, 10));
  return end != nullptr && *end == '\0' && *b >= 1;
}

}  // namespace

bool ParseFeedSpec(const std::string& spec, FeedConfig* out) {
  const size_t colon = spec.rfind(':');
  if (colon == std::string::npos || colon == 0) return false;
  FeedConfig c;
  c.device = spec.substr(0, colon);
  if (!ParseChannels(spec.substr(colon + 1), &c.ch_a, &c.ch_b)) return false;
  *out = c;
  return true;
}

std::string FormatFeedSpec(const FeedConfig& c) {
  char buf[32];
  if (c.ch_b > 0) {
    std::snprintf(buf, sizeof(buf), ":%d-%d", c.ch_a, c.ch_b);
  } else {
    std::snprintf(buf, sizeof(buf), ":%d", c.ch_a);
  }
  return c.device + buf;
}

bool ParseFeedLine(const std::string& line, FeedConfig* out) {
  const size_t c2 = line.rfind(',');
  if (c2 == std::string::npos || c2 == 0) return false;
  const size_t c1 = line.rfind(',', c2 - 1);
  if (c1 == std::string::npos) return false;
  FeedConfig c;
  if (!ParseFeedSpec(line.substr(0, c1), &c)) return false;
  c.gain_db = std::atof(line.substr(c1 + 1, c2 - c1 - 1).c_str());
  c.latch = line.substr(c2 + 1) == "1";
  *out = c;
  return true;
}

std::string FormatFeedLine(const FeedConfig& c) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), ",%.1f,%d", c.gain_db, c.latch ? 1 : 0);
  return FormatFeedSpec(c) + buf;
}

void ExtractDownmix(const float* interleaved, int frames, int channels,
                    int ch_a, int ch_b, float* mono_out) {
  if (channels < 1) channels = 1;
  int a = ch_a < 1 ? 1 : (ch_a > channels ? channels : ch_a);
  a -= 1;  // to 0-based
  if (ch_b < 1) {
    for (int i = 0; i < frames; ++i) {
      mono_out[i] = interleaved[i * channels + a];
    }
    return;
  }
  int b = ch_b > channels ? channels : ch_b;
  b -= 1;
  for (int i = 0; i < frames; ++i) {
    mono_out[i] = 0.5f * (interleaved[i * channels + a] +
                          interleaved[i * channels + b]);
  }
}

FeedChain::FeedChain(const FeedConfig& cfg)
    : cfg_(cfg),
      gain_(cfg.gain_db, 30.0, kSampleRate),
      // Same ceiling discipline as the mic chain: a console bus runs hot,
      // and clipping into Zoom's encoder is unrecoverable downstream.
      limiter_(-1.0, 2.0, 80.0, kSampleRate),
      latch_env_(12.0, kSampleRate),
      ring_(50) {
  latched_.store(cfg.latch);
}

void FeedChain::SetGainDb(double db) {
  cfg_.gain_db = db;
  gain_.set_db(db);
}

void FeedChain::PushInterleaved(const float* interleaved, int frames,
                                int channels) {
  if (interleaved == nullptr || frames <= 0) return;
  mono_.resize(static_cast<size_t>(frames));
  ExtractDownmix(interleaved, frames, channels, cfg_.ch_a, cfg_.ch_b,
                 mono_.data());
  gain_.Process(mono_.data(), frames);
  limiter_.Process(mono_.data(), frames);

  // LATCH is this source's envelope: the target comes from the control
  // thread's atomic, the ramp itself runs here, and once fully silent the
  // chain stops producing frames entirely -- the ring drains and the sink
  // sees "no feed" rather than an eternity of zeros.
  if (latched_.load()) {
    latch_env_.Open();
  } else {
    latch_env_.Close();
  }
  if (!latch_env_.open() && latch_env_.silent()) {
    latch_env_.Advance(frames);
    peak_.store(0);
    return;
  }
  latch_env_.Process(mono_.data(), frames);

  accum_.Push(mono_.data(), frames, [this](const float* frame, uint64_t seq) {
    TxFrame t;
    t.seq = seq;
    int pk = 0;
    for (int i = 0; i < kFrameSamples; ++i) {
      float v = frame[i] * 32767.0f;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      t.pcm[i] = static_cast<int16_t>(v);
      const int a = t.pcm[i] < 0 ? -t.pcm[i] : t.pcm[i];
      if (a > pk) pk = a;
    }
    ring_.Push(t);
    frames_out_.fetch_add(1);
    const int decayed = peak_.load() * 15 / 16;
    peak_.store(pk > decayed ? pk : decayed);
  });
}

bool FeedChain::PullFrame(int16_t* out) {
  TxFrame t;
  if (!ring_.Pop(&t)) return false;
  std::memcpy(out, t.pcm, sizeof(t.pcm));
  return true;
}

}  // namespace zc
