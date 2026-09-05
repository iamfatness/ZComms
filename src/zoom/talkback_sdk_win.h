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

#include <mutex>
#include <string>
#include <utility>
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
  TalkbackResult CreateChannels(unsigned int count) override;
  TalkbackResult InviteUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override;
  TalkbackResult RemoveUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override;
  TalkbackResult DestroyChannels(
      const std::vector<std::string>& channel_ids) override;
  TalkbackResult SendAudio(const std::string& channel_id, const int16_t* pcm,
                           int samples) override;
  TalkbackResult SetChannelBackgroundVolume(const std::string& channel_id,
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
  // Widen(id) is looked up from here instead of computed fresh. SendAudio
  // runs on the 50 Hz TX pacer thread (up to 16 channels x one 20 ms tick =
  // ~800 calls/s); a ~36-char GUID exceeds MSVC's wstring SSO (8 wchar_t),
  // so a fresh Widen() there is a guaranteed malloc+free per call, inside
  // send_m_ (TalkbackChannels), on the one thread this codebase is most
  // protective of. Capacity is reserved up front -- the SDK caps a meeting
  // at 16 channels -- so the backing vector never reallocates and a
  // reference handed back stays valid without the lock held.
  const std::wstring& WidenCached(const std::string& id);

  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller_;
  TalkbackSdkEvents* events_ = nullptr;

  std::mutex widen_cache_m_;
  std::vector<std::pair<std::string, std::wstring>> widen_cache_;
};

}  // namespace zc
