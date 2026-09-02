// Pins the extern-feed chain with no hardware: spec parsing (device names
// carry arbitrary punctuation -- fields split from the END), channel
// extract/downmix, and the latch envelope's produce/drain behavior.
#include <cmath>
#include <string>
#include <vector>

#include "audio_defs.h"
#include "extern_feed.h"
#include "test_util.h"

void TestExternFeed() {
  ZC_TEST("extern_feed: spec parse -- single channel");
  {
    zc::FeedConfig c;
    ZC_CHECK(zc::ParseFeedSpec("Dante Virtual Soundcard:5", &c));
    ZC_CHECK(c.device == "Dante Virtual Soundcard");
    ZC_CHECK(c.ch_a == 5 && c.ch_b == -1);
  }

  ZC_TEST("extern_feed: spec parse -- pair, device with colon and dash");
  {
    zc::FeedConfig c;
    ZC_CHECK(zc::ParseFeedSpec("USB Audio: Line-In (2):3-4", &c));
    ZC_CHECK(c.device == "USB Audio: Line-In (2)");
    ZC_CHECK(c.ch_a == 3 && c.ch_b == 4);
  }

  ZC_TEST("extern_feed: spec parse -- malformed rejected");
  {
    zc::FeedConfig c;
    ZC_CHECK(!zc::ParseFeedSpec("no-channels", &c));
    ZC_CHECK(!zc::ParseFeedSpec("dev:0", &c));
    ZC_CHECK(!zc::ParseFeedSpec("dev:a-b", &c));
    ZC_CHECK(!zc::ParseFeedSpec(":3", &c));
  }

  ZC_TEST("extern_feed: spec round-trips");
  {
    zc::FeedConfig c;
    ZC_CHECK(zc::ParseFeedSpec("GoXLR Chat:3-4", &c));
    ZC_CHECK(zc::FormatFeedSpec(c) == "GoXLR Chat:3-4");
  }

  ZC_TEST("extern_feed: feeds.env line round-trips gain and latch");
  {
    zc::FeedConfig c;
    c.device = "Dante, RX bank";  // a comma in the device name must survive
    c.ch_a = 7;
    c.ch_b = 8;
    c.gain_db = -6.0;
    c.latch = true;
    zc::FeedConfig back;
    ZC_CHECK(zc::ParseFeedLine(zc::FormatFeedLine(c), &back));
    ZC_CHECK(back.device == c.device);
    ZC_CHECK(back.ch_a == 7 && back.ch_b == 8);
    ZC_CHECK_NEAR(back.gain_db, -6.0, 0.05);
    ZC_CHECK(back.latch);
  }

  ZC_TEST("extern_feed: extract picks the right channel");
  {
    // 4-channel interleave: ch3 carries 0.5, everything else garbage.
    const int frames = 8;
    std::vector<float> in(frames * 4);
    for (int i = 0; i < frames; ++i) {
      in[i * 4 + 0] = 0.9f;
      in[i * 4 + 1] = -0.9f;
      in[i * 4 + 2] = 0.5f;
      in[i * 4 + 3] = 0.1f;
    }
    std::vector<float> mono(frames);
    zc::ExtractDownmix(in.data(), frames, 4, 3, -1, mono.data());
    ZC_CHECK_NEAR(mono[0], 0.5, 1e-6);
    ZC_CHECK_NEAR(mono[frames - 1], 0.5, 1e-6);
  }

  ZC_TEST("extern_feed: pair downmix averages, out-of-range clamps");
  {
    const int frames = 4;
    std::vector<float> in(frames * 2);
    for (int i = 0; i < frames; ++i) {
      in[i * 2 + 0] = 0.2f;
      in[i * 2 + 1] = 0.6f;
    }
    std::vector<float> mono(frames);
    zc::ExtractDownmix(in.data(), frames, 2, 1, 2, mono.data());
    ZC_CHECK_NEAR(mono[0], 0.4, 1e-6);
    // Channel 9 of a stereo device clamps to channel 2, not garbage memory.
    zc::ExtractDownmix(in.data(), frames, 2, 9, -1, mono.data());
    ZC_CHECK_NEAR(mono[0], 0.6, 1e-6);
  }

  ZC_TEST("extern_feed: latched chain frames audio; peak tracks it");
  {
    zc::FeedConfig c;
    c.device = "test";
    c.latch = true;
    zc::FeedChain chain(c);
    // 100 ms of a -12 dBFS tone on channel 1 of 2.
    const int frames = zc::kSampleRate / 10;
    std::vector<float> in(frames * 2);
    for (int i = 0; i < frames; ++i) {
      in[i * 2] = 0.25f * static_cast<float>(std::sin(
                              2.0 * 3.14159265 * 440.0 * i / zc::kSampleRate));
    }
    chain.PushInterleaved(in.data(), frames, 2);
    ZC_CHECK(chain.frames_out() >= 4);  // 100 ms = 5 frames, accumulator lag ok
    int16_t frame[zc::kFrameSamples];
    ZC_CHECK(chain.PullFrame(frame));
    ZC_CHECK(chain.peak() > 4000);  // ~0.25 fs minus ramp/limiter headroom
  }

  ZC_TEST("extern_feed: unlatched chain ramps out, then produces nothing");
  {
    zc::FeedConfig c;
    c.device = "test";
    c.latch = false;
    zc::FeedChain chain(c);
    const int frames = zc::kSampleRate / 10;
    std::vector<float> in(frames, 0.25f);
    chain.PushInterleaved(in.data(), frames, 1);
    ZC_CHECK(chain.frames_out() == 0);  // never latched -> nothing framed
    chain.SetLatch(true);
    chain.PushInterleaved(in.data(), frames, 1);
    const uint64_t latched_frames = chain.frames_out();
    ZC_CHECK(latched_frames >= 4);
    chain.SetLatch(false);
    // First push after unlatch carries the ramp-out tail...
    chain.PushInterleaved(in.data(), frames, 1);
    // ...but once silent, production stops entirely: the ring drains rather
    // than carrying an eternity of zeros.
    const uint64_t after_ramp = chain.frames_out();
    chain.PushInterleaved(in.data(), frames, 1);
    chain.PushInterleaved(in.data(), frames, 1);
    ZC_CHECK(chain.frames_out() == after_ramp);
  }

  ZC_TEST("extern_feed: input peak reads signal while UNLATCHED");
  {
    // The panel's whole reason for an input meter: confirm a source is
    // arriving BEFORE committing it to air. peak() is measured after the
    // latch envelope and is therefore zero whenever the feed is down, which
    // is exactly when the operator needs to see level. input_peak() is the
    // pre-envelope tap, and it must read the same signal either way.
    zc::FeedConfig c;
    c.device = "test";
    c.latch = false;
    zc::FeedChain chain(c);
    const int frames = zc::kSampleRate / 10;
    std::vector<float> in(frames * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
      in[i * 2] = 0.25f * static_cast<float>(std::sin(
                              2.0 * 3.14159265 * 440.0 * i / zc::kSampleRate));
    }
    chain.PushInterleaved(in.data(), frames, 2);
    ZC_CHECK(chain.frames_out() == 0);   // nothing goes to air
    ZC_CHECK(chain.peak() == 0);         // post-envelope: correctly silent
    ZC_CHECK(chain.input_peak() > 4000); // pre-envelope: the source IS there

    // And it follows the gain the operator is riding, so the meter and the
    // control beside it agree.
    const int unity = chain.input_peak();
    chain.SetGainDb(-20.0);
    for (int i = 0; i < 20; ++i) chain.PushInterleaved(in.data(), frames, 2);
    ZC_CHECK(chain.input_peak() < unity / 2);

    // A silent source reads silent, so the meter cannot lie the other way.
    chain.SetGainDb(0.0);
    std::vector<float> quiet(frames * 2, 0.0f);
    for (int i = 0; i < 40; ++i) chain.PushInterleaved(quiet.data(), frames, 2);
    ZC_CHECK(chain.input_peak() < 100);
  }
}
