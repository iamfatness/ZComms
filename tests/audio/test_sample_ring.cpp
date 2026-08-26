#include <vector>

#include "sample_ring.h"
#include "test_util.h"

using namespace zc;

void TestSampleRing() {
  std::printf("TestSampleRing\n");

  {
    ZC_TEST("round-trips in order");
    SampleRing r(100);
    std::vector<float> in{1.0f, 2.0f, 3.0f};
    r.Write(in.data(), in.size());
    std::vector<float> out(3, 0.0f);
    ZC_CHECK(r.Read(out.data(), 3) == 3);
    ZC_CHECK(out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f);
    ZC_CHECK(r.size() == 0);
  }

  {
    ZC_TEST("bounded: overflow drops oldest and counts");
    // Unbounded buffering would turn sidetone into a growing delay, and
    // hearing your own voice late is the artefact that actively interferes
    // with speaking.
    SampleRing r(4);
    std::vector<float> in{1, 2, 3, 4, 5, 6};
    r.Write(in.data(), in.size());
    ZC_CHECK(r.size() == 4);
    ZC_CHECK(r.dropped() == 2);
    std::vector<float> out(4, 0.0f);
    r.Read(out.data(), 4);
    ZC_CHECK(out[0] == 3.0f);  // 1 and 2 dropped
  }

  {
    ZC_TEST("underflow zero-fills and counts rather than returning junk");
    SampleRing r(100);
    std::vector<float> in{7.0f, 8.0f};
    r.Write(in.data(), in.size());
    std::vector<float> out(5, 99.0f);
    ZC_CHECK(r.Read(out.data(), 5) == 2);
    ZC_CHECK(out[0] == 7.0f && out[1] == 8.0f);
    for (size_t i = 2; i < out.size(); ++i) ZC_CHECK(out[i] == 0.0f);
    ZC_CHECK(r.starved() == 3);
  }

  {
    ZC_TEST("survives wrapping repeatedly");
    SampleRing r(64);
    float v = 0.0f;
    for (int round = 0; round < 200; ++round) {
      std::vector<float> in(48);
      for (auto& x : in) x = v++;
      r.Write(in.data(), in.size());
      std::vector<float> out(48, 0.0f);
      r.Read(out.data(), 48);
    }
    ZC_CHECK(r.size() <= 64);
  }
}
