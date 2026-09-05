#include "fake_mic_sender.h"
#include "test_util.h"

using zctest::FakeVirtualMic;

void TestVirtualMic() {
  ZC_TEST("a send before onMicInitialize is refused");
  {
    // Zoom hands the sender over in onMicInitialize. Before that there is
    // nothing to send into, and the pacer must be told so rather than
    // discovering it by dereferencing null on the 20 ms tick.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.accepted.empty());
  }

  ZC_TEST("initialised is not the same as sending");
  {
    // The two states answer different operator questions: no sender at all
    // (usually a missing raw-data entitlement) versus a sender whose window
    // is shut (usually a muted meeting mic). Collapsing them loses the
    // distinction that tells the operator what to fix.
    FakeVirtualMic mic;
    mic.Initialize();
    ZC_CHECK(mic.initialised());
    ZC_CHECK(!mic.sending());
    ZC_CHECK(!mic.CanSend());
  }

  ZC_TEST("send is legal only between StartSend and StopSend");
  {
    // The law, from CLAUDE.md: send() is legal only between onMicStartSend
    // and onMicStopSend.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {7};
    mic.Initialize();
    ZC_CHECK(!mic.Send(pcm, 160));      // window shut
    mic.StartSend();
    ZC_CHECK(mic.CanSend());
    ZC_CHECK(mic.Send(pcm, 160));       // window open
    ZC_CHECK(mic.accepted.size() == 1u);
    mic.StopSend();
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));      // window shut again
    ZC_CHECK(mic.accepted.size() == 1u);
  }

  ZC_TEST("a revoked sender refuses sends, it does not crash");
  {
    // onMicUninitialized drops the pointer. A TX thread that had already
    // passed CanSend() must find a closed gate rather than a revoked
    // pointer -- that failure is a crash, not a glitch, and a 20 ms cadence
    // finds it quickly.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    ZC_CHECK(mic.Send(pcm, 160));
    mic.Uninitialize();
    ZC_CHECK(!mic.initialised());
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.accepted.size() == 1u);
  }

  ZC_TEST("a refused send is counted and its code kept");
  {
    // fails=0 alone cannot distinguish "Zoom accepted audio" from "nothing
    // was ever sent" -- the 2026-08-29 no-audio hunt stalled on exactly
    // that ambiguity in the talkback path.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    mic.fail_with = 3;
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.send_failures() == 1u);
    ZC_CHECK(mic.last_error() == 3);
    ZC_CHECK(mic.accepted.empty());
  }

  ZC_TEST("re-initialising reopens the mic from a clean state");
  {
    // Zoom can tear the virtual mic down and hand it back within one
    // session. A stale can_send_ across that boundary would make the pacer
    // send into a sender Zoom has not opened yet.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    mic.Uninitialize();
    mic.Initialize();
    ZC_CHECK(mic.initialised());
    ZC_CHECK(!mic.sending());
    ZC_CHECK(!mic.Send(pcm, 160));
    mic.StartSend();
    ZC_CHECK(mic.Send(pcm, 160));
  }
}
