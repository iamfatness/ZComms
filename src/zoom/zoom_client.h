// SDK lifecycle: init, authenticate, join, install the virtual mic.
//
// Everything here runs on the thread that called InitSDK, and that thread
// pumps the Windows message queue. The Windows Meeting SDK delivers its
// callbacks through the message loop, so a harness that simply slept would
// authenticate and join exactly never -- which presents as a silent hang and
// is a genuinely confusing first failure.
//
// The TX thread is separate and is the only thread that touches send(), which
// is also plan §5's rule about never running media on the thread that handles
// control.
#pragma once

// See mic_source.h: the SDK headers depend on windows.h having been included.
// clang-format off
#include <windows.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "auth_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_interface.h"
#include "mic_source.h"
#include "zoom_sdk.h"

namespace zc {

class ZoomClient : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent,
                   public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent,
                   public ZOOM_SDK_NAMESPACE::IMeetingAudioCtrlEvent {
 public:
  ~ZoomClient();

  bool Init(std::string* error);

  // Uses the public app key if present, otherwise mints a JWT from
  // sdk_key/sdk_secret. Blocks (pumping messages) until the SDK answers.
  bool Authenticate(const std::string& public_app_key, const std::string& sdk_key,
                    const std::string& sdk_secret, int timeout_ms,
                    std::string* error);

  bool Join(uint64_t meeting_number, const std::string& password,
            const std::string& display_name, int timeout_ms, std::string* error);

  // setExternalAudioSource. This one call is the entire TX path (plan §2).
  bool InstallVirtualMic(ZoomMicSource* source, std::string* error);

  bool JoinVoip(std::string* error);
  bool LeaveVoip(std::string* error);

  // Unmutes this client in the meeting. A meeting with mute-on-entry admits
  // the harness muted, and a muted client's virtual mic never receives
  // onMicStartSend -- the send window simply stays shut. Host-side unmute of
  // an SDK client can require a consent handshake this harness does not
  // implement, so it unmutes itself.
  bool UnmuteSelf(std::string* error);

  // Logs this client's audio connection and mute state, so "the send window
  // never opened" comes with the reason attached instead of being a mystery.
  void LogSelfAudioState(const char* tag);

  // The talkback controller for this meeting, or null.
  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* GetTalkbackController();

  ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* GetParticipantsController();

  // Every participant except this client -- the people a talkback channel
  // would be addressed to.
  std::vector<unsigned int> GetOtherParticipants();

  void Leave();
  void Cleanup();

  // Runs the message loop for `ms`, which is how SDK callbacks get delivered.
  // Anything that waits in this harness waits by calling this.
  void Pump(int ms);

  ZOOM_SDK_NAMESPACE::MeetingStatus status() const { return status_.load(); }
  bool in_meeting() const {
    return status_.load() == ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING;
  }

  // IAuthServiceEvent
  void onAuthenticationReturn(ZOOM_SDK_NAMESPACE::AuthResult ret) override;
  void onLoginReturnWithReason(ZOOM_SDK_NAMESPACE::LOGINSTATUS ret,
                               ZOOM_SDK_NAMESPACE::IAccountInfo* info,
                               ZOOM_SDK_NAMESPACE::LoginFailReason reason) override;
  void onLogout() override;
  void onZoomIdentityExpired() override;
  void onZoomAuthIdentityExpired() override;
  void onNotificationServiceStatus(
      ZOOM_SDK_NAMESPACE::SDKNotificationServiceStatus status,
      ZOOM_SDK_NAMESPACE::SDKNotificationServiceError error) override;

  // IMeetingServiceEvent
  void onMeetingStatusChanged(ZOOM_SDK_NAMESPACE::MeetingStatus status,
                              int result) override;
  void onMeetingStatisticsWarningNotification(
      ZOOM_SDK_NAMESPACE::StatisticsWarningType type) override;
  void onMeetingParameterNotification(
      const ZOOM_SDK_NAMESPACE::MeetingParameter* param) override;
  void onSuspendParticipantsActivities() override;
  void onAICompanionActiveChangeNotice(bool active) override;
  void onMeetingTopicChanged(const zchar_t* topic) override;
  void onMeetingFullToWatchLiveStream(const zchar_t* url) override;
  void onUserNetworkStatusChanged(ZOOM_SDK_NAMESPACE::MeetingComponentType type,
                                  ZOOM_SDK_NAMESPACE::ConnectionQuality level,
                                  unsigned int user_id, bool uplink) override;
  void onAppSignalPanelUpdated(
      ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler* handler) override;

  // IMeetingAudioCtrlEvent
  void onUserAudioStatusChange(
      ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::IUserAudioStatus*>* list,
      const zchar_t* json) override;
  void onUserActiveAudioChange(
      ZOOM_SDK_NAMESPACE::IList<unsigned int>* list) override;
  void onHostRequestStartAudio(
      ZOOM_SDK_NAMESPACE::IRequestStartAudioHandler* handler) override;
  void onJoin3rdPartyTelephonyAudio(const zchar_t* audio_info) override;
  void onMuteOnEntryStatusChange(bool enabled) override;

 private:
  ZOOM_SDK_NAMESPACE::IAuthService* auth_ = nullptr;
  ZOOM_SDK_NAMESPACE::IMeetingService* meeting_ = nullptr;
  std::atomic<ZOOM_SDK_NAMESPACE::MeetingStatus> status_{
      ZOOM_SDK_NAMESPACE::MEETING_STATUS_IDLE};
  std::atomic<int> auth_result_{-1};
  std::atomic<bool> auth_returned_{false};
  std::atomic<int> last_fail_code_{0};
  bool sdk_initialised_ = false;
};

// Human-readable forms. A raw enum ordinal in a failure message costs whoever
// reads it a trip to the headers, and join failures are the most common wall.
const char* MeetingStatusName(ZOOM_SDK_NAMESPACE::MeetingStatus s);
const char* AuthResultName(ZOOM_SDK_NAMESPACE::AuthResult r);
std::string MeetingFailReason(int code);

}  // namespace zc
