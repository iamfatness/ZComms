#include "zoom_client.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "clock.h"
#include "jwt.h"
// meeting_service_interface.h forward-declares IMeetingAudioController but does
// not define it, so JoinVoip() needs the component header explicitly.
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"
#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
#include "meeting_service_components/meeting_waiting_room_interface.h"
#include "rawdata/zoom_rawdata_api.h"

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

}  // namespace

const char* MeetingStatusName(MeetingStatus s) {
  switch (s) {
    case MEETING_STATUS_IDLE: return "IDLE";
    case MEETING_STATUS_CONNECTING: return "CONNECTING";
    case MEETING_STATUS_WAITINGFORHOST: return "WAITING_FOR_HOST";
    case MEETING_STATUS_INMEETING: return "IN_MEETING";
    case MEETING_STATUS_DISCONNECTING: return "DISCONNECTING";
    case MEETING_STATUS_RECONNECTING: return "RECONNECTING";
    case MEETING_STATUS_FAILED: return "FAILED";
    case MEETING_STATUS_ENDED: return "ENDED";
    case MEETING_STATUS_LOCKED: return "LOCKED";
    case MEETING_STATUS_UNLOCKED: return "UNLOCKED";
    case MEETING_STATUS_IN_WAITING_ROOM: return "IN_WAITING_ROOM";
    case MEETING_STATUS_WEBINAR_PROMOTE: return "WEBINAR_PROMOTE";
    case MEETING_STATUS_WEBINAR_DEPROMOTE: return "WEBINAR_DEPROMOTE";
    case MEETING_STATUS_JOIN_BREAKOUT_ROOM: return "JOIN_BREAKOUT_ROOM";
    case MEETING_STATUS_LEAVE_BREAKOUT_ROOM: return "LEAVE_BREAKOUT_ROOM";
    default: return "UNKNOWN";
  }
}

const char* AuthResultName(AuthResult r) {
  switch (r) {
    case AUTHRET_SUCCESS: return "SUCCESS";
    case AUTHRET_KEYORSECRETEMPTY: return "KEY_OR_SECRET_EMPTY";
    case AUTHRET_KEYORSECRETWRONG: return "KEY_OR_SECRET_WRONG";
    case AUTHRET_ACCOUNTNOTSUPPORT: return "ACCOUNT_NOT_SUPPORTED";
    case AUTHRET_ACCOUNTNOTENABLESDK: return "ACCOUNT_NOT_ENABLED_FOR_SDK";
    case AUTHRET_UNKNOWN: return "UNKNOWN";
    case AUTHRET_SERVICE_BUSY: return "SERVICE_BUSY";
    case AUTHRET_NONE: return "NONE";
    case AUTHRET_OVERTIME: return "TIMEOUT";
    case AUTHRET_NETWORKISSUE: return "NETWORK_ISSUE";
    case AUTHRET_CLIENT_INCOMPATIBLE: return "CLIENT_INCOMPATIBLE";
    case AUTHRET_JWTTOKENWRONG: return "JWT_TOKEN_WRONG";
    case AUTHRET_LIMIT_EXCEEDED_EXCEPTION: return "RATE_LIMIT_EXCEEDED";
    default: return "?";
  }
}

std::string MeetingFailReason(int code) {
  switch (code) {
    case MEETING_FAIL_PASSWORD_ERR: return "wrong passcode";
    case MEETING_FAIL_MEETING_NOT_START: return "meeting has not started";
    case MEETING_FAIL_MEETING_NOT_EXIST: return "meeting does not exist";
    case MEETING_FAIL_MEETING_OVER: return "meeting is over";
    case MEETING_FAIL_MEETING_USER_FULL: return "meeting is full";
    case MEETING_FAIL_CONNECTION_ERR: return "connection error";
    case MEETING_FAIL_MMR_ERR: return "media server error";
    case MEETING_FAIL_CONFLOCKED: return "meeting is locked";
    case MEETING_FAIL_MEETING_RESTRICTED: return "meeting is restricted";
    case MEETING_FAIL_ENFORCE_LOGIN:
      return "meeting requires a signed-in user -- a guest SDK client cannot "
             "join it";
    case MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING:
      return "private meeting, sign-in required";
    case MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN:
      return "blocked by account admin";
    case MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING:
      // The PKCE app identity's boundary, hit live 2026-08-29: guest joins
      // are only allowed into meetings on the Zoom account that authorized
      // this app. Anything else fails here, before any passcode business.
      return "this app can only join meetings hosted by its own Zoom account "
             "-- start the meeting from the account that installed ZComms";
    case MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN:
      return "the host's account does not allow outside participants";
    case MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING:
      return "this account is not allowed to join external meetings";
    case ZoomClient::kFailAccountBusyElsewhere:
      return "your Zoom account is in a meeting on another device -- ZComms "
             "joins as you; leave that meeting first (or join one you are "
             "not hosting)";
    default: return "code " + std::to_string(code);
  }
}

ZoomClient::~ZoomClient() { Cleanup(); }

void ZoomClient::Pump(int ms) {
  const int64_t deadline = NowNs() + static_cast<int64_t>(ms) * 1'000'000;
  MSG msg;
  do {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    Sleep(1);
  } while (NowNs() < deadline);
}

bool ZoomClient::Init(std::string* error) {
  InitParam param;
  param.strWebDomain = L"https://zoom.us";
  param.strSupportUrl = L"https://zoom.us";
  param.emLanguageID = LANGUAGE_English;
  param.enableLogByDefault = true;
  param.enableGenerateDump = true;
  // Heap rather than the stack default. Audio raw data buffers handed to a
  // callback that outlives the callback frame are a use-after-free waiting to
  // happen; the harness does not subscribe to RX today, but the setting is
  // free and the failure it prevents is not.
  param.rawdataOpts.audioRawdataMemoryMode = ZoomSDKRawDataMemoryModeHeap;

  const SDKError err = InitSDK(param);
  if (err != SDKERR_SUCCESS) {
    *error = "InitSDK failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  sdk_initialised_ = true;
  std::printf("[sdk] initialised, version %ls\n", GetSDKVersion());
  return true;
}

bool ZoomClient::AuthenticateWithJwt(const std::string& jwt, int timeout_ms,
                                     std::string* error) {
  if (CreateAuthService(&auth_) != SDKERR_SUCCESS || auth_ == nullptr) {
    *error = "CreateAuthService failed";
    return false;
  }
  auth_->SetEvent(this);

  AuthContext ctx;
  const std::wstring jwt_w = Widen(jwt);
  ctx.jwt_token = jwt_w.c_str();
  std::printf("[sdk] authenticating with a broker-minted SDK JWT\n");

  const SDKError err = auth_->SDKAuth(ctx);
  if (err != SDKERR_SUCCESS) {
    *error = "SDKAuth call failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  const int64_t deadline = NowNs() + static_cast<int64_t>(timeout_ms) * 1'000'000;
  while (!auth_returned_.load() && NowNs() < deadline) Pump(50);
  if (!auth_returned_.load()) {
    *error = "authentication timed out after " + std::to_string(timeout_ms) + " ms";
    return false;
  }
  const AuthResult result = static_cast<AuthResult>(auth_result_.load());
  if (result != AUTHRET_SUCCESS) {
    *error = std::string("authentication failed: ") + AuthResultName(result);
    return false;
  }
  return true;
}

bool ZoomClient::Authenticate(const std::string& public_app_key,
                              const std::string& sdk_key,
                              const std::string& sdk_secret, int timeout_ms,
                              std::string* error) {
  if (CreateAuthService(&auth_) != SDKERR_SUCCESS || auth_ == nullptr) {
    *error = "CreateAuthService failed";
    return false;
  }
  auth_->SetEvent(this);

  AuthContext ctx;
  std::wstring key_w, jwt_w;

  if (!public_app_key.empty()) {
    key_w = Widen(public_app_key);
    ctx.publicAppKey = key_w.c_str();
    std::printf("[sdk] authenticating with public app key (...%s)\n",
                public_app_key.size() > 4
                    ? public_app_key.substr(public_app_key.size() - 4).c_str()
                    : "****");
  } else {
    std::string jwt_err;
    const std::string jwt = MakeMeetingSdkJwt(sdk_key, sdk_secret, 3600, &jwt_err);
    if (jwt.empty()) {
      *error = "no credentials: " + jwt_err;
      return false;
    }
    jwt_w = Widen(jwt);
    ctx.jwt_token = jwt_w.c_str();
    std::printf("[sdk] authenticating with a locally-minted JWT\n");
  }

  const SDKError err = auth_->SDKAuth(ctx);
  if (err != SDKERR_SUCCESS) {
    *error = "SDKAuth call failed: " + std::to_string(static_cast<int>(err));
    return false;
  }

  const int64_t deadline = NowNs() + static_cast<int64_t>(timeout_ms) * 1'000'000;
  while (!auth_returned_.load() && NowNs() < deadline) Pump(50);

  if (!auth_returned_.load()) {
    *error = "authentication timed out after " + std::to_string(timeout_ms) + " ms";
    return false;
  }
  const AuthResult result = static_cast<AuthResult>(auth_result_.load());
  if (result != AUTHRET_SUCCESS) {
    *error = std::string("authentication failed: ") + AuthResultName(result);
    return false;
  }
  return true;
}

bool ZoomClient::Join(uint64_t meeting_number, const std::string& password,
                      const std::string& display_name, int timeout_ms,
                      std::string* error, const std::function<void()>& on_tick,
                      const std::string& zak) {
  if (CreateMeetingService(&meeting_) != SDKERR_SUCCESS || meeting_ == nullptr) {
    *error = "CreateMeetingService failed";
    return false;
  }
  meeting_->SetEvent(this);
  display_name_ = display_name;
  passcode_state_.store(0);
  pw_handler_ = nullptr;
  last_fail_code_.store(0);
  // Without this listener a passcode-protected meeting joined by bare ID
  // fails opaquely -- the SDK asks for the passcode via callback and gives
  // up when nobody answers.
  if (IMeetingConfiguration* mc = meeting_->GetMeetingConfiguration()) {
    mc->SetEvent(this);
  }

  const std::wstring name_w = Widen(display_name);
  const std::wstring pw_w = Widen(password);
  const std::wstring zak_w = Widen(zak);

  JoinParam jp;
  jp.userType = SDK_UT_WITHOUT_LOGIN;
  JoinParam4WithoutLogin& p = jp.param.withoutloginuserJoin;
  p.meetingNumber = meeting_number;
  p.userName = name_w.c_str();
  p.psw = pw_w.c_str();
  if (!zak_w.empty()) p.userZAK = zak_w.c_str();
  p.isVideoOff = true;
  // Audio explicitly ON: the whole point is a virtual mic on the meeting's
  // audio, and joining with audio off would install a mic onto nothing.
  p.isAudioOff = false;

  const SDKError err = meeting_->Join(jp);
  if (err != SDKERR_SUCCESS) {
    *error = "Join call failed: " + std::to_string(static_cast<int>(err));
    return false;
  }

  const int64_t deadline = NowNs() + static_cast<int64_t>(timeout_ms) * 1'000'000;
  while (NowNs() < deadline) {
    Pump(100);
    if (on_tick) on_tick();
    const MeetingStatus s = status_.load();
    if (s == MEETING_STATUS_INMEETING) return true;
    if (s == MEETING_STATUS_FAILED) {
      *error = "join failed: " + MeetingFailReason(last_fail_code_.load());
      return false;
    }
    if (s == MEETING_STATUS_ENDED) {
      const int code = last_fail_code_.load();
      *error = code != 0 ? "join failed: " + MeetingFailReason(code)
                         : "the meeting ended before we got in";
      return false;
    }
  }

  // A waiting room is not a bug and not a timeout -- it is a host action the
  // operator has to take, and saying so is the difference between a 30-second
  // fix and a debugging session.
  const MeetingStatus s = status_.load();
  if (s == MEETING_STATUS_IN_WAITING_ROOM) {
    *error = "still in the waiting room -- the host must admit \"" +
             display_name + "\"";
  } else if (s == MEETING_STATUS_WAITINGFORHOST) {
    *error = "waiting for the host to start the meeting";
  } else {
    *error = std::string("join timed out in state ") + MeetingStatusName(s);
  }
  return false;
}

bool ZoomClient::JoinVoip(std::string* error) {
  if (meeting_ == nullptr) {
    *error = "no meeting service";
    return false;
  }
  IMeetingAudioController* audio = meeting_->GetMeetingAudioController();
  if (audio == nullptr) {
    *error = "GetMeetingAudioController returned null";
    return false;
  }
  // Register for audio events before joining, so the very first
  // onUserAudioStatusChange is observed rather than missed.
  audio->SetEvent(this);
  const SDKError err = audio->JoinVoip();
  if (err != SDKERR_SUCCESS) {
    *error = "JoinVoip failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

bool ZoomClient::LeaveVoip(std::string* error) {
  if (meeting_ == nullptr) {
    *error = "no meeting service";
    return false;
  }
  IMeetingAudioController* audio = meeting_->GetMeetingAudioController();
  if (audio == nullptr) {
    *error = "GetMeetingAudioController returned null";
    return false;
  }
  const SDKError err = audio->LeaveVoip();
  if (err != SDKERR_SUCCESS) {
    *error = "LeaveVoip failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

bool ZoomClient::SelfMuted() {
  if (meeting_ == nullptr) return false;
  IMeetingParticipantsController* parts =
      meeting_->GetMeetingParticipantsController();
  IUserInfo* self = parts != nullptr ? parts->GetMySelfUser() : nullptr;
  return self != nullptr && self->IsAudioMuted();
}

bool ZoomClient::UnmuteSelf(std::string* error) {
  if (meeting_ == nullptr) {
    *error = "no meeting service";
    return false;
  }
  IMeetingParticipantsController* parts =
      meeting_->GetMeetingParticipantsController();
  if (parts == nullptr) {
    *error = "GetMeetingParticipantsController returned null";
    return false;
  }
  IUserInfo* self = parts->GetMySelfUser();
  if (self == nullptr) {
    *error = "GetMySelfUser returned null";
    return false;
  }
  if (!self->IsAudioMuted()) return true;  // nothing to do

  IMeetingAudioController* audio = meeting_->GetMeetingAudioController();
  if (audio == nullptr) {
    *error = "GetMeetingAudioController returned null";
    return false;
  }
  const SDKError err = audio->UnMuteAudio(self->GetUserID());
  if (err != SDKERR_SUCCESS) {
    // A meeting configured "participants cannot unmute" lands here, and there
    // is no fix on our side -- plan §2 calls this out. Name it rather than
    // leaving a bare error code.
    *error = "UnMuteAudio failed: " + std::to_string(static_cast<int>(err)) +
             " (if the meeting forbids self-unmute, the host must allow it)";
    return false;
  }
  return true;
}

bool ZoomClient::InstallVirtualMic(ZoomMicSource* source, std::string* error) {
  if (!HasRawdataLicense()) {
    // Worth checking explicitly. Without the raw-data entitlement the calls
    // below can succeed and simply never fire a callback, which looks like a
    // hang rather than like a licensing problem.
    std::printf("[sdk] WARNING: HasRawdataLicense() is false -- the virtual "
                "mic callbacks may never fire\n");
  }
  IZoomSDKAudioRawDataHelper* helper = GetAudioRawdataHelper();
  if (helper == nullptr) {
    *error = "GetAudioRawdataHelper returned null";
    return false;
  }
  const SDKError err = helper->setExternalAudioSource(source);
  if (err != SDKERR_SUCCESS) {
    *error = "setExternalAudioSource failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

void ZoomClient::Leave() {
  if (meeting_ != nullptr && status_.load() == MEETING_STATUS_INMEETING) {
    meeting_->Leave(LEAVE_MEETING);
    for (int i = 0; i < 30 && status_.load() == MEETING_STATUS_INMEETING; ++i) {
      Pump(100);
    }
  }
}

void ZoomClient::Cleanup() {
  // Teardown order is deliberate: services first, SDK last. CleanUPSDK() is
  // documented as unsafe from inside a callback, which is one more reason the
  // pump is drained before getting here.
  if (meeting_ != nullptr) {
    DestroyMeetingService(meeting_);
    meeting_ = nullptr;
  }
  if (auth_ != nullptr) {
    DestroyAuthService(auth_);
    auth_ = nullptr;
  }
  if (sdk_initialised_) {
    CleanUPSDK();
    sdk_initialised_ = false;
  }
}

// --- IAuthServiceEvent ------------------------------------------------------

void ZoomClient::onAuthenticationReturn(AuthResult ret) {
  auth_result_.store(static_cast<int>(ret));
  auth_returned_.store(true);
  std::printf("[sdk] onAuthenticationReturn: %s\n", AuthResultName(ret));
}

void ZoomClient::onLoginReturnWithReason(LOGINSTATUS, IAccountInfo*,
                                         LoginFailReason) {}
void ZoomClient::onLogout() {}
void ZoomClient::onZoomIdentityExpired() {
  std::printf("[sdk] zoom identity expired\n");
}
void ZoomClient::onZoomAuthIdentityExpired() {
  std::printf("[sdk] zoom auth identity expires soon\n");
}
void ZoomClient::onNotificationServiceStatus(SDKNotificationServiceStatus,
                                             SDKNotificationServiceError) {}

// --- IMeetingServiceEvent ---------------------------------------------------

void ZoomClient::onMeetingStatusChanged(MeetingStatus status, int result) {
  status_.store(status);
  // FAILED carries the real reason; the ENDED that follows it carries 0 and
  // must not clobber it (live: FAILED 504 -> ENDED 0 reported as "code 0").
  if (status == MEETING_STATUS_FAILED ||
      (status == MEETING_STATUS_ENDED && last_fail_code_.load() == 0)) {
    last_fail_code_.store(result);
  }
  std::printf("[sdk] meeting status: %s%s\n", MeetingStatusName(status),
              (status == MEETING_STATUS_FAILED)
                  ? (" (" + MeetingFailReason(result) + ")").c_str()
                  : "");
}

void ZoomClient::onInputMeetingPasswordAndScreenNameNotification(
    IMeetingPasswordAndScreenNameHandler* handler) {
  if (!handler) return;
  const auto type = handler->GetRequiredInfoType();
  if (type == IMeetingPasswordAndScreenNameHandler::REQUIRED_INFO_TYPE_Password ||
      type == IMeetingPasswordAndScreenNameHandler::
                  REQUIRED_INFO_TYPE_Password4WrongPassword ||
      type == IMeetingPasswordAndScreenNameHandler::
                  REQUIRED_INFO_TYPE_PasswordAndScreenName) {
    pw_handler_ = handler;
    const bool wrong =
        type == IMeetingPasswordAndScreenNameHandler::
                    REQUIRED_INFO_TYPE_Password4WrongPassword;
    passcode_state_.store(wrong ? 2 : 1);
    std::printf("[sdk] meeting requires a passcode%s\n",
                wrong ? " (previous one was wrong)" : "");
  } else if (type == IMeetingPasswordAndScreenNameHandler::
                         REQUIRED_INFO_TYPE_ScreenName) {
    // We always have a name; answer inline and move on.
    handler->InputMeetingScreenName(Widen(display_name_).c_str());
  } else {
    handler->Cancel();
  }
}

// A guest join can be asked to identify itself (name + email) before entering
// -- common on meetings created outside this account. Nobody is at a dialog
// to answer, so answer inline; ignoring the prompt kills the join with an
// opaque ENDED.
void ZoomClient::onJoinMeetingNeedUserInfo(
    IMeetingInputUserInfoHandler* handler) {
  if (!handler) return;
  std::printf("[sdk] meeting wants name+email; answering as \"%s\"\n",
              display_name_.c_str());
  const SDKError e = handler->InputUserInfo(Widen(display_name_).c_str(),
                                            L"operator@zcomms.app");
  if (e != SDKERR_SUCCESS) {
    std::printf("[sdk] InputUserInfo failed: %d\n", static_cast<int>(e));
  }
}

// The remaining prompts have no sane automatic answer; name them loudly so a
// stuck join says why instead of dying as an opaque ENDED.
void ZoomClient::onWebinarNeedRegisterNotification(
    IWebinarNeedRegisterHandler*) {
  std::printf("[sdk] this webinar requires registration -- cannot join\n");
}
void ZoomClient::onEndOtherMeetingToJoinMeetingNotification(
    IEndOtherMeetingToJoinMeetingHandler* handler) {
  // ZComms joins AS the signed-in operator (ZAK), so if that account is
  // already hosting a meeting elsewhere -- their own Zoom client in their
  // PMI, live 2026-08-29 -- the SDK asks "end the other meeting to join?".
  // Unanswered, the join hangs FOREVER, which read as "the app isn't
  // responding". Never end the operator's own meeting out from under them:
  // Cancel, fail the join, and say why. The status stores a code the Join
  // wait turns into operator language.
  std::printf("[sdk] this account is already in a meeting elsewhere -- "
              "declining to end it; join cancelled\n");
  if (handler != nullptr) handler->Cancel();
  last_fail_code_.store(kFailAccountBusyElsewhere);
  status_.store(MEETING_STATUS_FAILED);
}
void ZoomClient::onWebinarNeedInputScreenName(IWebinarInputScreenNameHandler*) {
  std::printf("[sdk] webinar wants a screen name prompt\n");
}

bool ZoomClient::SubmitPasscode(const std::string& passcode) {
  IMeetingPasswordAndScreenNameHandler* h = pw_handler_;
  if (!h || passcode.empty()) return false;
  // The SDK destroys the handler inside this call; a wrong passcode comes
  // back as a fresh notification with the Password4WrongPassword type.
  pw_handler_ = nullptr;
  passcode_state_.store(0);
  return h->InputMeetingPasswordAndScreenName(Widen(passcode).c_str(),
                                              Widen(display_name_).c_str());
}

void ZoomClient::onMeetingStatisticsWarningNotification(StatisticsWarningType) {}
void ZoomClient::onMeetingParameterNotification(const MeetingParameter*) {}
void ZoomClient::onSuspendParticipantsActivities() {}
void ZoomClient::onAICompanionActiveChangeNotice(bool) {}
void ZoomClient::onMeetingTopicChanged(const zchar_t*) {}
void ZoomClient::onMeetingFullToWatchLiveStream(const zchar_t*) {}
void ZoomClient::onUserNetworkStatusChanged(MeetingComponentType,
                                            ConnectionQuality, unsigned int,
                                            bool) {}
void ZoomClient::onAppSignalPanelUpdated(IMeetingAppSignalHandler*) {}

IMeetingTalkbackController* ZoomClient::GetTalkbackController() {
  return meeting_ != nullptr ? meeting_->GetMeetingTalkbackController() : nullptr;
}

IMeetingParticipantsController* ZoomClient::GetParticipantsController() {
  return meeting_ != nullptr ? meeting_->GetMeetingParticipantsController()
                             : nullptr;
}

bool ZoomClient::AdmitAllWaiting() {
  if (meeting_ == nullptr) return false;
  IMeetingWaitingRoomController* wr = meeting_->GetMeetingWaitingRoomController();
  if (wr == nullptr) return false;
  return wr->AdmitAllToMeeting() == SDKERR_SUCCESS;
}

std::vector<unsigned int> ZoomClient::GetOtherParticipants() {
  std::vector<unsigned int> out;
  if (meeting_ == nullptr) return out;
  IMeetingParticipantsController* parts =
      meeting_->GetMeetingParticipantsController();
  if (parts == nullptr) return out;
  IUserInfo* self = parts->GetMySelfUser();
  const unsigned int self_id = self != nullptr ? self->GetUserID() : 0;
  IList<unsigned int>* list = parts->GetParticipantsList();
  if (list == nullptr) return out;
  for (int i = 0; i < list->GetCount(); ++i) {
    const unsigned int uid = list->GetItem(i);
    if (uid != self_id) out.push_back(uid);
  }
  return out;
}

// --- IMeetingAudioCtrlEvent -------------------------------------------------

void ZoomClient::LogSelfAudioState(const char* tag) {
  if (meeting_ == nullptr) return;
  IMeetingParticipantsController* parts =
      meeting_->GetMeetingParticipantsController();
  IUserInfo* self = parts != nullptr ? parts->GetMySelfUser() : nullptr;
  if (self == nullptr) {
    std::printf("[audio] %s: self unavailable\n", tag);
    return;
  }
  std::printf("[audio] %s: user %u, muted=%s\n", tag, self->GetUserID(),
              self->IsAudioMuted() ? "YES" : "no");
}

void ZoomClient::onUserAudioStatusChange(IList<IUserAudioStatus*>* list,
                                         const zchar_t*) {
  if (list == nullptr) return;
  static const char* kStatus[] = {"none",          "muted",
                                  "unmuted",       "muted-by-host",
                                  "unmuted-by-host", "muted-all",
                                  "unmuted-all"};
  static const char* kType[] = {"NONE", "VOIP", "PHONE", "UNKNOWN"};
  // Per-user, per-event: in a live meeting this callback fires on every
  // mute/unmute by ANYONE, with the whole affected list -- printed, that
  // was a console scroll storm ("logs are going crazy", owner, live
  // 2026-08-29). Log ONLY this client's own transitions; everyone else's
  // mute state is roster noise this station does not act on.
  for (int i = 0; i < list->GetCount(); ++i) {
    IUserAudioStatus* s = list->GetItem(i);
    if (s == nullptr) continue;
    IMeetingParticipantsController* pc =
        meeting_ ? meeting_->GetMeetingParticipantsController() : nullptr;
    IUserInfo* self = pc ? pc->GetMySelfUser() : nullptr;
    if (self == nullptr || s->GetUserId() != self->GetUserID()) continue;
    const int st = static_cast<int>(s->GetStatus());
    const int ty = static_cast<int>(s->GetAudioType());
    std::printf("[audio] self status=%s type=%s\n",
                (st >= 0 && st <= 6) ? kStatus[st] : "?",
                (ty >= 0 && ty <= 3) ? kType[ty] : "?");
  }
}

void ZoomClient::onUserActiveAudioChange(IList<unsigned int>*) {}

void ZoomClient::onHostRequestStartAudio(IRequestStartAudioHandler* handler) {
  // The host clicking "ask to unmute" arrives here as a consent request, not
  // as an unmute. The first live run sat muted for five minutes while the
  // operator repeatedly unmuted it in the participant list, because nothing
  // accepted -- so a headless harness accepts by policy and says so.
  std::printf("[audio] host asked to start audio -- accepting\n");
  if (handler != nullptr) handler->Accept();
}

void ZoomClient::onJoin3rdPartyTelephonyAudio(const zchar_t*) {}

void ZoomClient::onMuteOnEntryStatusChange(bool enabled) {
  std::printf("[audio] mute-on-entry is %s\n", enabled ? "ON" : "off");
}

}  // namespace zc
