// A TalkbackSdk that records instead of calling Zoom.
//
// Everything the ladder does to the SDK lands in calls[]; everything the SDK
// would say back is scripted with next_result / Emit*. No timing, no threads:
// these tests pin decisions, not schedules.
#pragma once

#include <string>
#include <vector>

#include "talkback_channels.h"
#include "talkback_sdk.h"

namespace zctest {

struct FakeCall {
  std::string op;                     // "create", "invite", "remove", "send", "volume"
  std::string channel_id;
  std::vector<unsigned int> user_ids;
  float volume = 0.0f;
};

class FakeTalkbackSdk : public zc::TalkbackSdk {
 public:
  std::vector<FakeCall> calls;
  zc::TalkbackCall next_result = zc::TalkbackCall::Ok;
  bool supports = true;

  void SetEvents(zc::TalkbackSdkEvents* events) override { events_ = events; }
  bool MeetingSupportsTalkback() override { return supports; }

  zc::TalkbackResult CreateChannels(unsigned int count) override {
    FakeCall c;
    c.op = "create";
    c.user_ids.push_back(count);
    calls.push_back(c);
    return next_result;
  }

  zc::TalkbackResult InviteUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override {
    calls.push_back({"invite", channel_id, user_ids, 0.0f});
    return next_result;
  }

  zc::TalkbackResult RemoveUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override {
    calls.push_back({"remove", channel_id, user_ids, 0.0f});
    return next_result;
  }

  zc::TalkbackResult DestroyChannels(
      const std::vector<std::string>& channel_ids) override {
    for (const std::string& id : channel_ids) {
      calls.push_back({"destroy", id, {}, 0.0f});
    }
    return next_result;
  }

  zc::TalkbackResult SendAudio(const std::string& channel_id, const int16_t*,
                             int samples) override {
    FakeCall c;
    c.op = "send";
    c.channel_id = channel_id;
    c.user_ids.push_back(static_cast<unsigned int>(samples));
    calls.push_back(c);
    return next_result;
  }

  zc::TalkbackResult SetChannelBackgroundVolume(const std::string& channel_id,
                                              float volume) override {
    calls.push_back({"volume", channel_id, {}, volume});
    return next_result;
  }

  // Drive the ladder's callbacks the way Zoom would.
  void EmitChannelCreated(const std::string& id) {
    events_->OnCreateChannelResponse(id, zc::TalkbackEvent::Ok);
  }
  void EmitUserJoined(const std::string& id, unsigned int user_id,
                      zc::TalkbackEvent e = zc::TalkbackEvent::Ok) {
    events_->OnChannelUserJoinResponse(id, user_id, e);
  }

  int CountOp(const std::string& op) const {
    int n = 0;
    for (const FakeCall& c : calls) {
      if (c.op == op) ++n;
    }
    return n;
  }

 private:
  zc::TalkbackSdkEvents* events_ = nullptr;
};

}  // namespace zctest
