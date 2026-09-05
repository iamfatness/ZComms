#include "talkback_sdk_win.h"

#include "audio_defs.h"

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::string Narrow(const zchar_t* s) {
  if (s == nullptr) return "";
  std::string out;
  for (const zchar_t* p = s; *p != 0; ++p) {
    out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // channel ids are ASCII GUIDs
}

// SDKERR_TOO_FREQUENT_CALL is enum position 18. Law 2's backoff turns on
// exactly this value and nothing above the seam may see the raw number.
TalkbackCall FromSdkError(SDKError err) {
  switch (err) {
    case SDKERR_SUCCESS: return TalkbackCall::Ok;
    case SDKERR_TOO_FREQUENT_CALL: return TalkbackCall::TooFrequent;
    case SDKERR_WRONG_USAGE: return TalkbackCall::WrongUsage;
    default: return TalkbackCall::Failed;
  }
}

TalkbackEvent FromTalkbackError(
    IMeetingTalkbackCtrlEvent::TalkbackError e) {
  switch (e) {
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_OK: return TalkbackEvent::Ok;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION:
      return TalkbackEvent::NoPermission;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_ALREADY_EXIST:
      return TalkbackEvent::AlreadyExists;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_COUNT_OVERFLOW:
      return TalkbackEvent::CountOverflow;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOT_EXIST:
      return TalkbackEvent::NotExist;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_REJECTED:
      return TalkbackEvent::Rejected;
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_TIMEOUT:
      return TalkbackEvent::Timeout;
    default: return TalkbackEvent::Unknown;
  }
}

}  // namespace

TalkbackSdkWin::TalkbackSdkWin(IMeetingTalkbackController* controller)
    : controller_(controller) {
  if (controller_ != nullptr) controller_->SetEvent(this);
}

void TalkbackSdkWin::SetEvents(TalkbackSdkEvents* events) { events_ = events; }

bool TalkbackSdkWin::MeetingSupportsTalkback() {
  return controller_ != nullptr && controller_->IsMeetingSupportTalkBack();
}

TalkbackCall TalkbackSdkWin::CreateChannels(unsigned int count) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  return FromSdkError(controller_->CreateChannel(count));
}

TalkbackCall TalkbackSdkWin::InviteUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  const std::wstring idw = Widen(channel_id);
  SDKError err = controller_->BeginBatchInviteUsers(idw.c_str());
  for (size_t i = 0; i < user_ids.size() && err == SDKERR_SUCCESS; ++i) {
    err = controller_->AddUserToInvite(user_ids[i]);
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchInviteUsers();
  return FromSdkError(err);
}

TalkbackCall TalkbackSdkWin::RemoveUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  const std::wstring idw = Widen(channel_id);
  SDKError err = controller_->BeginBatchRemoveUsers(idw.c_str());
  for (size_t i = 0; i < user_ids.size() && err == SDKERR_SUCCESS; ++i) {
    err = controller_->AddUserToRemove(user_ids[i]);
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchRemoveUsers();
  return FromSdkError(err);
}

TalkbackCall TalkbackSdkWin::DestroyChannels(
    const std::vector<std::string>& channel_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (channel_ids.empty()) return TalkbackCall::Ok;
  // Verified against the real header (meeting_talkback_ctrl_interface.h):
  // there is no single-shot DestroyChannel(id). Destruction is a
  // Begin/Add/Execute batch, same shape as invite/remove -- mirrored here
  // rather than the single-call loop, per the brief's fallback note.
  // TalkbackChannels never calls this (the bank is provisioned once and
  // never torn down), so it is unexercised in production either way.
  SDKError err = controller_->BeginBatchDestroyChannels();
  for (size_t i = 0; i < channel_ids.size() && err == SDKERR_SUCCESS; ++i) {
    const std::wstring idw = Widen(channel_ids[i]);
    err = controller_->AddChannelToDestroy(idw.c_str());
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchDestroyChannels();
  return FromSdkError(err);
}

TalkbackCall TalkbackSdkWin::SendAudio(const std::string& channel_id,
                                       const int16_t* pcm, int samples) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  const std::wstring idw = Widen(channel_id);
  return FromSdkError(controller_->SendAudioDataToChannel(
      idw.c_str(), reinterpret_cast<const char*>(pcm),
      static_cast<unsigned int>(samples * static_cast<int>(sizeof(int16_t))),
      kSampleRate, ZoomSDKAudioChannel_Mono));
}

TalkbackCall TalkbackSdkWin::SetChannelBackgroundVolume(
    const std::string& channel_id, float volume) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  const std::wstring idw = Widen(channel_id);
  return FromSdkError(
      controller_->SetChannelBackgroundVolume(idw.c_str(), volume));
}

void TalkbackSdkWin::onCreateChannelResponse(const zchar_t* channel_id,
                                             TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnCreateChannelResponse(Narrow(channel_id),
                                     FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onDestroyChannelResponse(const zchar_t* channel_id,
                                              TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnDestroyChannelResponse(Narrow(channel_id),
                                      FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onChannelUserJoinResponse(const zchar_t* channel_id,
                                               unsigned int user_id,
                                               TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnChannelUserJoinResponse(Narrow(channel_id), user_id,
                                       FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onChannelUserLeaveResponse(const zchar_t* channel_id,
                                                unsigned int user_id,
                                                TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnChannelUserLeaveResponse(Narrow(channel_id), user_id,
                                        FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onJoinTalkbackChannel(unsigned int inviter_id) {
  if (events_ != nullptr) events_->OnJoinTalkbackChannel(inviter_id);
}

void TalkbackSdkWin::onLeaveTalkbackChannel(unsigned int inviter_id) {
  if (events_ != nullptr) events_->OnLeaveTalkbackChannel(inviter_id);
}

}  // namespace zc
