// Pins the per-channel TX composer: the routing truth table, the barge
// duck's signal gating, its ramp, and the clamp at the int16 boundary.
#include <vector>

#include "audio_defs.h"
#include "channel_mix.h"
#include "test_util.h"

namespace {

std::vector<int16_t> Frame(int16_t value) {
  return std::vector<int16_t>(zc::kFrameSamples, value);
}

}  // namespace

void TestChannelMix() {
  const auto voice = Frame(1000);
  const auto feed = Frame(10000);
  std::vector<int16_t> out(zc::kFrameSamples);

  ZC_TEST("channel_mix: nothing in, nothing to send");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    ZC_CHECK(!m.Compose(0, nullptr, nullptr, false, zc::kFrameSamples,
                        out.data()));
  }

  ZC_TEST("channel_mix: voice alone passes through untouched");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    ZC_CHECK(m.Compose(0, voice.data(), nullptr, true, zc::kFrameSamples,
                       out.data()));
    ZC_CHECK(out[0] == 1000 && out[zc::kFrameSamples - 1] == 1000);
  }

  ZC_TEST("channel_mix: latched feed alone at unity");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    ZC_CHECK(m.Compose(0, nullptr, feed.data(), false, zc::kFrameSamples,
                       out.data()));
    ZC_CHECK(out[0] == 10000 && out[zc::kFrameSamples - 1] == 10000);
  }

  ZC_TEST("channel_mix: keyed-but-silent voice does NOT duck the feed");
  {
    // The ZoomISO refinement: key state alone must not dent the feed.
    zc::ChannelMix m(50.0, zc::kSampleRate);
    const auto silent_voice = Frame(0);
    ZC_CHECK(m.Compose(0, silent_voice.data(), feed.data(),
                       /*voice_active=*/false, zc::kFrameSamples, out.data()));
    ZC_CHECK(out[zc::kFrameSamples - 1] == 10000);
    ZC_CHECK_NEAR(m.barge_gain(0), 1.0, 1e-6);
  }

  ZC_TEST("channel_mix: active voice ducks the feed to 30% within the ramp");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    // 50 ms ramp = 2.5 frames; after 3 frames the duck must be complete.
    for (int i = 0; i < 3; ++i) {
      m.Compose(0, voice.data(), feed.data(), true, zc::kFrameSamples,
                out.data());
    }
    ZC_CHECK_NEAR(m.barge_gain(0), zc::ChannelMix::kBargeDuck, 1e-4);
    // 1000 + 10000 * 0.3 = 4000.
    ZC_CHECK(out[zc::kFrameSamples - 1] >= 3990 &&
             out[zc::kFrameSamples - 1] <= 4010);
  }

  ZC_TEST("channel_mix: duck releases back to unity when voice goes quiet");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    for (int i = 0; i < 3; ++i) {
      m.Compose(0, voice.data(), feed.data(), true, zc::kFrameSamples,
                out.data());
    }
    for (int i = 0; i < 3; ++i) {
      m.Compose(0, nullptr, feed.data(), false, zc::kFrameSamples, out.data());
    }
    ZC_CHECK_NEAR(m.barge_gain(0), 1.0, 1e-4);
    ZC_CHECK(out[zc::kFrameSamples - 1] == 10000);
  }

  ZC_TEST("channel_mix: the ramp is gradual, not a step");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    m.Compose(0, voice.data(), feed.data(), true, zc::kFrameSamples,
              out.data());
    // First sample still near unity, last sample of frame 1 well on the way
    // down -- the transition lives inside the audio, not between frames.
    ZC_CHECK(out[0] > 10500);
    ZC_CHECK(out[zc::kFrameSamples - 1] < out[0]);
  }

  ZC_TEST("channel_mix: hot voice + hot feed clamps, never wraps");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    const auto hot_v = Frame(30000);
    const auto hot_f = Frame(30000);
    ZC_CHECK(m.Compose(0, hot_v.data(), hot_f.data(), false, zc::kFrameSamples,
                       out.data()));
    ZC_CHECK(out[0] == 32767);  // clamped, not wrapped negative
  }

  ZC_TEST("channel_mix: slots ramp independently");
  {
    zc::ChannelMix m(50.0, zc::kSampleRate);
    for (int i = 0; i < 3; ++i) {
      m.Compose(0, voice.data(), feed.data(), true, zc::kFrameSamples,
                out.data());
    }
    ZC_CHECK(m.Compose(1, nullptr, feed.data(), true, zc::kFrameSamples,
                       out.data()));
    ZC_CHECK(out[0] == 10000);  // slot 1 never ducked
    ZC_CHECK_NEAR(m.barge_gain(1), 1.0, 1e-6);
  }
}
