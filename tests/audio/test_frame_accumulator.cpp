#include <vector>

#include "audio_defs.h"
#include "frame_accumulator.h"
#include "test_util.h"

using namespace zc;

void TestFrameAccumulator() {
  std::printf("TestFrameAccumulator\n");

  {
    ZC_TEST("emits exact 20 ms frames from ragged block sizes");
    // Devices hand over whatever they like, and not the same size twice. The
    // TX cadence is what Zoom is being fed on, so it cannot inherit that.
    FrameAccumulator acc;
    int frames = 0;
    const int blocks[] = {441, 1024, 100, 7, 4096, 480, 333};
    uint64_t total_in = 0;
    for (int n : blocks) {
      std::vector<float> b(static_cast<size_t>(n), 0.5f);
      total_in += static_cast<uint64_t>(n);
      acc.Push(b.data(), n, [&](const float*, uint64_t) { ++frames; });
    }
    ZC_CHECK(acc.samples_in() == total_in);
    ZC_CHECK(static_cast<uint64_t>(frames) == total_in / kFrameSamples);
    ZC_CHECK(acc.pending() == static_cast<int>(total_in % kFrameSamples));
  }

  {
    ZC_TEST("loses no samples and keeps them in order");
    // A dropped or reordered sample here would be an audible glitch with no
    // counter anywhere to explain it.
    FrameAccumulator acc;
    std::vector<float> out;
    int counter = 0;
    for (int block = 0; block < 40; ++block) {
      const int n = 137;  // deliberately coprime with the frame size
      std::vector<float> b(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) b[static_cast<size_t>(i)] =
          static_cast<float>(counter++);
      acc.Push(b.data(), n, [&](const float* f, uint64_t) {
        out.insert(out.end(), f, f + kFrameSamples);
      });
    }
    for (size_t i = 0; i < out.size(); ++i) {
      ZC_CHECK(out[i] == static_cast<float>(i));
    }
  }

  {
    ZC_TEST("frame start index is derived from samples, not calls");
    // Plan section 5: callback arrival jitters, sample counts do not.
    FrameAccumulator acc;
    std::vector<uint64_t> starts;
    std::vector<float> b(static_cast<size_t>(kFrameSamples) * 5, 0.0f);
    acc.Push(b.data(), static_cast<int>(b.size()),
             [&](const float*, uint64_t s) { starts.push_back(s); });
    ZC_CHECK(starts.size() == 5);
    for (size_t i = 0; i < starts.size(); ++i) {
      ZC_CHECK(starts[i] == i * static_cast<uint64_t>(kFrameSamples));
    }
  }

  {
    ZC_TEST("degenerate input does not crash");
    FrameAccumulator acc;
    acc.Push(nullptr, 100, [](const float*, uint64_t) {});
    std::vector<float> b(10, 0.0f);
    acc.Push(b.data(), 0, [](const float*, uint64_t) {});
    acc.Push(b.data(), -5, [](const float*, uint64_t) {});
    ZC_CHECK(acc.frames_out() == 0);
  }
}
