// TalkbackSdk over the Windows Meeting SDK.
//
// This file owns two things nothing above it should know: the
// Begin/Add/Execute batch sequences (whose mutual-exclusion rules produced a
// Major), and the zchar_t conversions. Channel ids are ASCII GUIDs, so
// widening is a straight char-by-char copy.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <string>
#include <vector>

#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
#include "talkback_sdk.h"

namespace zc {

class TalkbackSdkWin : public TalkbackSdk,
                       public ZOOM_SDK_NAMESPACE::IMeetingTalkbackCtrlEvent {
 public:
  explicit TalkbackSdkWin(
      ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller);

  void SetEvents(TalkbackSdkEvents* events) override;
  bool MeetingSupportsTalkback() override;
  TalkbackCall CreateChannels(unsigned int count) override;
  TalkbackCall InviteUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackCall RemoveUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackCall DestroyChannels(
      const std::vector<std::string>& channel_ids) override;
  TalkbackCall SendAudio(const std::string& channel_id, const int16_t* pcm,
                         int samples) override;
  TalkbackCall SetChannelBackgroundVolume(const std::string& channel_id,
                                          float volume) override;

  // IMeetingTalkbackCtrlEvent -- forwarded to events_.
  void onCreateChannelResponse(const zchar_t* channel_id,
                               TalkbackError error) override;
  void onDestroyChannelResponse(const zchar_t* channel_id,
                                TalkbackError error) override;
  void onChannelUserJoinResponse(const zchar_t* channel_id,
                                 unsigned int user_id,
                                 TalkbackError error) override;
  void onChannelUserLeaveResponse(const zchar_t* channel_id,
                                  unsigned int user_id,
                                  TalkbackError error) override;
  void onJoinTalkbackChannel(unsigned int inviter_id) override;
  void onLeaveTalkbackChannel(unsigned int inviter_id) override;
  void onInviterAudioLevel(unsigned int, unsigned int) override {}

 private:
  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller_;
  TalkbackSdkEvents* events_ = nullptr;
};

}  // namespace zc
