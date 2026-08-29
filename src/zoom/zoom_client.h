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
#include <functional>
#include <string>
#include <vector>

#include "auth_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_configuration_interface.h"
#include "meeting_service_interface.h"
#include "mic_source.h"
#include "zoom_sdk.h"

namespace zc {

class ZoomClient : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent,
                   public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent,
                   public ZOOM_SDK_NAMESPACE::IMeetingAudioCtrlEvent,
                   public ZOOM_SDK_NAMESPACE::IMeetingConfigurationEvent {
 public:
  ~ZoomClient();

  bool Init(std::string* error);

  // Uses the public app key if present, otherwise mints a JWT from
  // sdk_key/sdk_secret. Blocks (pumping messages) until the SDK answers.
  bool Authenticate(const std::string& public_app_key, const std::string& sdk_key,
                    const std::string& sdk_secret, int timeout_ms,
                    std::string* error);

  // Auth with a ready-made Meeting SDK JWT (the broker mints it from the
  // operator's OAuth session). This is the signed-in path -- paired with a
  // ZAK on Join, the client is the operator's account, not an anonymous
  // guest, which is what lifts the cross-account join refusal (fail 504).
  bool AuthenticateWithJwt(const std::string& jwt, int timeout_ms,
                           std::string* error);

  // on_tick (optional) runs every pump iteration of the join wait, so the
  // caller can surface progress -- and, when the meeting demands a passcode
  // the operator didn't provide, collect one -- without owning the loop.
  // zak, when non-empty, joins as the signed-in user (JoinParam userZAK).
  bool Join(uint64_t meeting_number, const std::string& password,
            const std::string& display_name, int timeout_ms, std::string* error,
            const std::function<void()>& on_tick = nullptr,
            const std::string& zak = std::string());

  // Passcode conversation, driven by onInputMeetingPasswordAndScreenName-
  // Notification. A bare meeting ID on a passcode-protected meeting used to
  // die as a misleading "meeting ended" join failure -- the SDK was asking
  // for the passcode and nobody was listening.
  //   0 = not asked, 1 = passcode needed, 2 = passcode was wrong, ask again.
  int passcode_state() const { return passcode_state_.load(); }
  bool SubmitPasscode(const std::string& passcode);

  // Local (non-Zoom) failure code: the signed-in account is already in a
  // meeting on another device and we refused to end it. Chosen outside
  // Zoom's MeetingFailCode range.
  static constexpr int kFailAccountBusyElsewhere = 909001;

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

  // Admits everyone currently in the waiting room. Host/co-host only; a
  // no-permission failure is expected when not host and is not an error worth
  // surfacing every tick.
  bool AdmitAllWaiting();

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

  // IMeetingConfigurationEvent (only the passcode prompt matters here)
  void onInputMeetingPasswordAndScreenNameNotification(
      ZOOM_SDK_NAMESPACE::IMeetingPasswordAndScreenNameHandler* handler)
      override;
  void onWebinarNeedRegisterNotification(
      ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandler*) override;
  void onEndOtherMeetingToJoinMeetingNotification(
      ZOOM_SDK_NAMESPACE::IEndOtherMeetingToJoinMeetingHandler*) override;
  void onWebinarNeedInputScreenName(
      ZOOM_SDK_NAMESPACE::IWebinarInputScreenNameHandler*) override;
  void onJoinMeetingNeedUserInfo(
      ZOOM_SDK_NAMESPACE::IMeetingInputUserInfoHandler* handler) override;
  void onUserConfirmToStartArchive(
      ZOOM_SDK_NAMESPACE::IMeetingArchiveConfirmHandler*) override {}
  void onUserConfirmRecoverMeeting(
      ZOOM_SDK_NAMESPACE::IMeetingConfirmRecoverHandler*) override {}
  // IMeetingConfigurationFreeMeetingEvent (inherited noise)
  void onFreeMeetingRemainTime(unsigned int) override {}
  void onFreeMeetingRemainTimeStopCountDown() override {}
  void onFreeMeetingNeedToUpgrade(FreeMeetingNeedUpgradeType,
                                  const zchar_t*) override {}
  void onFreeMeetingUpgradeToGiftFreeTrialStart() override {}
  void onFreeMeetingUpgradeToGiftFreeTrialStop() override {}
  void onFreeMeetingUpgradeToProMeeting() override {}

 private:
  ZOOM_SDK_NAMESPACE::IAuthService* auth_ = nullptr;
  ZOOM_SDK_NAMESPACE::IMeetingService* meeting_ = nullptr;
  std::atomic<ZOOM_SDK_NAMESPACE::MeetingStatus> status_{
      ZOOM_SDK_NAMESPACE::MEETING_STATUS_IDLE};
  std::atomic<int> auth_result_{-1};
  std::atomic<bool> auth_returned_{false};
  std::atomic<int> last_fail_code_{0};
  bool sdk_initialised_ = false;

  // The pending passcode prompt. The handler is only touched on the SDK's
  // callback thread (which is the pumping thread), and the SDK destroys it
  // the moment Input...() or Cancel() is called.
  ZOOM_SDK_NAMESPACE::IMeetingPasswordAndScreenNameHandler* pw_handler_ =
      nullptr;
  std::atomic<int> passcode_state_{0};
  std::string display_name_;
};

// Human-readable forms. A raw enum ordinal in a failure message costs whoever
// reads it a trip to the headers, and join failures are the most common wall.
const char* MeetingStatusName(ZOOM_SDK_NAMESPACE::MeetingStatus s);
const char* AuthResultName(ZOOM_SDK_NAMESPACE::AuthResult r);
std::string MeetingFailReason(int code);

}  // namespace zc
