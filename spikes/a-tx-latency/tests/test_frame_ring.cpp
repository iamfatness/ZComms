#include "frame_ring.h"
#include "test_util.h"

using namespace zc;

void TestFrameRing() {
  std::printf("TestFrameRing\n");

  {
    ZC_TEST("FIFO order and counts");
    FrameRing r(4);
    for (uint64_t i = 0; i < 3; ++i) {
      TxFrame f;
      f.seq = i;
      r.Push(f);
    }
    ZC_CHECK(r.size() == 3);
    TxFrame out;
    for (uint64_t i = 0; i < 3; ++i) {
      ZC_CHECK(r.Pop(&out));
      ZC_CHECK(out.seq == i);
    }
    ZC_CHECK(!r.Pop(&out));
    ZC_CHECK(r.drops() == 0);
  }

  {
    ZC_TEST("overflow drops the oldest and counts it");
    // Plan §6.2's contract. Dropping the newest instead would mean a stalled
    // consumer permanently serves stale audio, which in a talkback path is
    // worse than a gap.
    FrameRing r(4);
    for (uint64_t i = 0; i < 6; ++i) {
      TxFrame f;
      f.seq = i;
      r.Push(f);
    }
    ZC_CHECK(r.size() == 4);
    ZC_CHECK(r.drops() == 2);
    TxFrame out;
    ZC_CHECK(r.Pop(&out));
    ZC_CHECK(out.seq == 2);  // 0 and 1 were dropped
  }

  {
    ZC_TEST("burst markers survive the round trip");
    // The emission timestamp is keyed off these; losing them would silently
    // reduce the sample count with no other symptom.
    FrameRing r(2);
    TxFrame f;
    f.burst_id = 7;
    f.burst_up = false;
    f.burst_offset = 123;
    r.Push(f);
    TxFrame out;
    ZC_CHECK(r.Pop(&out));
    ZC_CHECK(out.burst_id == 7);
    ZC_CHECK(out.burst_up == false);
    ZC_CHECK(out.burst_offset == 123);
  }
}
