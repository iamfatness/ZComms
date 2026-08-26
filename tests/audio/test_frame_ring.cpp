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
    // Plan section 6.2. Dropping the newest instead would mean a stalled
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
    ZC_CHECK(out.seq == 2);
  }

  {
    ZC_TEST("markers survive the round trip");
    FrameRing r(2);
    TxFrame f;
    f.mark_id = 7;
    f.mark_flag = false;
    f.mark_offset = 123;
    r.Push(f);
    TxFrame out;
    ZC_CHECK(r.Pop(&out));
    ZC_CHECK(out.mark_id == 7);
    ZC_CHECK(out.mark_flag == false);
    ZC_CHECK(out.mark_offset == 123);
  }

  {
    ZC_TEST("audio payload survives intact");
    // The ring is the last thing between processed audio and Zoom; a payload
    // bug here would sound like corruption with no counter to explain it.
    FrameRing r(2);
    TxFrame f;
    for (int i = 0; i < kFrameSamples; ++i) {
      f.pcm[i] = static_cast<int16_t>((i * 37) % 32767 - 16000);
    }
    r.Push(f);
    TxFrame out;
    ZC_CHECK(r.Pop(&out));
    bool same = true;
    for (int i = 0; i < kFrameSamples; ++i) {
      if (out.pcm[i] != f.pcm[i]) { same = false; break; }
    }
    ZC_CHECK(same);
  }
}
