#include "fake_talkback_sdk.h"
#include "test_util.h"

using zc::TalkbackCall;
using zc::TalkbackChannels;
using zc::TalkbackEvent;
using zctest::FakeTalkbackSdk;

namespace {

// Bring N channels up and confirm them, the way Zoom does: one create call,
// then one response per channel in arrival order.
void BringUp(FakeTalkbackSdk* fake, TalkbackChannels* ch, int n) {
  std::string err;
  ch->CreateChannels(n, &err);
  for (int i = 0; i < n; ++i) {
    fake->EmitChannelCreated("guid-" + std::to_string(i));
  }
}

}  // namespace

void TestTalkbackChannels() {
  ZC_TEST("create asks for every channel in ONE call");
  {
    // N channels as N calls is what tripped the rate limit live on
    // 2026-08-29 with a 12-person roster.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ZC_CHECK(ch.CreateChannels(8, &err));
    ZC_CHECK(fake.CountOp("create") == 1);
    ZC_CHECK(fake.calls[0].user_ids[0] == 8u);
  }

  ZC_TEST("a channel is not ready until Zoom confirms it");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ch.CreateChannels(2, &err);
    ZC_CHECK(ch.channels_ready() == 0);
    fake.EmitChannelCreated("guid-0");
    ZC_CHECK(ch.channels_ready() == 1);
    fake.EmitChannelCreated("guid-1");
    ZC_CHECK(ch.channels_ready() == 2);
  }

  ZC_TEST("invite refuses a channel that is not ready");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ch.CreateChannels(1, &err);
    ZC_CHECK(!ch.Invite(0, 101, &err));
    ZC_CHECK(fake.CountOp("invite") == 0);
  }

  ZC_TEST("InviteMany sends everyone in ONE call");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    std::string err;
    ZC_CHECK(ch.InviteMany(0, {101, 102, 103}, &err));
    ZC_CHECK(fake.CountOp("invite") == 1);
    ZC_CHECK(fake.calls.back().user_ids.size() == 3u);
  }

  ZC_TEST("a channel refuses an eleventh member");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    std::string err;
    std::vector<unsigned int> eleven;
    for (unsigned int i = 0; i < 11; ++i) eleven.push_back(100 + i);
    ZC_CHECK(!ch.InviteMany(0, eleven, &err));
    ZC_CHECK(fake.CountOp("invite") == 0);
  }

  ZC_TEST("a rate-limited invite is reported, not swallowed");
  {
    // Law 2: the caller must learn it was refused so it can back off and
    // retry the SAME item. Reporting success here would drop the member.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.next_result = TalkbackCall::TooFrequent;
    std::string err;
    ZC_CHECK(!ch.InviteMany(0, {101}, &err));
    ZC_CHECK(!err.empty());
  }

  ZC_TEST("a confirmed join is recorded as a member");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.EmitUserJoined("guid-0", 101);
    ZC_CHECK(ch.Snapshot()[0].members.count(101) == 1u);
  }

  ZC_TEST("ALREADY_EXIST does NOT record presence -- pins a known bug");
  {
    // This pins what the code DOES, which is not what it should do.
    //
    // A member is recorded only on TalkbackEvent::Ok; every other response
    // just sets last_error_. So when Zoom answers an invite with
    // ALREADY_EXIST -- meaning the person IS in the channel -- the ladder
    // does not record them, `want && !have` stays true, and the healer
    // re-invites the same person every 5-60s for the life of the session,
    // spending the rate-limit budget that Law 2 exists to protect.
    //
    // It contradicts talkback_sdk.h's own contract ("Confirmed presence.
    // NEVER retried"). Fixing it is a BEHAVIOUR change and this plan is a
    // move-only port, so the fix is filed separately and needs its own live
    // verification. Owner ruling 2026-09-05: pin reality, file the bug.
    // When that fix lands, this test inverts to == 1u and the comment goes.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.EmitUserJoined("guid-0", 101, TalkbackEvent::AlreadyExists);
    ZC_CHECK(ch.Snapshot()[0].members.count(101) == 0u);
  }

  ZC_TEST("audio goes ONLY to keyed, ready channels");
  {
    // The routing rule is the entire product: a frame reaches a channel if
    // and only if that channel is keyed.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 3);
    const int16_t pcm[160] = {0};
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 0);
    ZC_CHECK(fake.CountOp("send") == 0);
    ch.SetKey(1, true);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 1);
    ZC_CHECK(fake.calls.back().channel_id == "guid-1");
    ch.SetKey(2, true);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 2);
    ch.SetKey(1, false);
    ch.SetKey(2, false);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 0);
  }

  ZC_TEST("a refused send is counted, not silently dropped");
  {
    // fails=0 alone could not distinguish "Zoom accepted audio" from
    // "nothing was ever sent" -- the 2026-08-29 no-audio hunt stalled there.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    ch.SetKey(0, true);
    fake.next_result = TalkbackCall::Failed;
    const int16_t pcm[160] = {0};
    ch.SendToKeyed(pcm, 160);
    ZC_CHECK(ch.send_failures() == 1u);
    ZC_CHECK(ch.channel_sends() == 0u);
    ZC_CHECK(ch.sent_mask() == 0u);
  }
}
