#include "talkback_source.h"

#include <cstdio>

#include "audio_defs.h"

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::string Narrow(const zchar_t* s) {
  if (s == nullptr) return "";
  std::string out;
  for (const zchar_t* p = s; *p != 0; ++p) {
    // Channel IDs are GUID-shaped ASCII; anything beyond that is replaced
    // rather than mis-encoded.
    out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // ASCII channel ids only
}

}  // namespace

const char* ZoomTalkbackSource::ErrorName(TalkbackError e) {
  switch (e) {
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_OK: return "OK";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOPERMISSION:
      return "NO_PERMISSION (talkback needs host/co-host and, per the ZoomISO "
             "lineage, an Enhanced Media entitled host account)";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_ALREADY_EXIST: return "ALREADY_EXIST";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_COUNT_OVERFLOW:
      return "COUNT_OVERFLOW (max 16 channels)";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_NOT_EXIST: return "NOT_EXIST";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_REJECTED: return "REJECTED";
    case IMeetingTalkbackCtrlEvent::TALKBACK_ERROR_TIMEOUT: return "TIMEOUT";
    default: return "UNKNOWN";
  }
}

ZoomTalkbackSource::ZoomTalkbackSource(IMeetingTalkbackController* controller)
    : controller_(controller) {
  if (controller_ != nullptr) controller_->SetEvent(this);
}

bool ZoomTalkbackSource::meeting_supports_talkback() const {
  return controller_ != nullptr && controller_->IsMeetingSupportTalkBack();
}

bool ZoomTalkbackSource::CreateChannel(std::string* error) {
  if (controller_ == nullptr) {
    *error = "GetMeetingTalkbackController returned null";
    return false;
  }
  const SDKError err = controller_->CreateChannel(1);
  if (err != SDKERR_SUCCESS) {
    *error = "CreateChannel failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

bool ZoomTalkbackSource::InviteUsers(const std::vector<unsigned int>& user_ids,
                                     std::string* error) {
  std::string id = channel_id();
  if (id.empty()) {
    *error = "no channel to invite into";
    return false;
  }
  const std::wstring id_w = Widen(id);
  SDKError err = controller_->BeginBatchInviteUsers(id_w.c_str());
  if (err != SDKERR_SUCCESS) {
    *error = "BeginBatchInviteUsers failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  for (unsigned int uid : user_ids) {
    err = controller_->AddUserToInvite(uid);
    if (err != SDKERR_SUCCESS) {
      std::printf("[talkback] AddUserToInvite(%u) failed: %d\n", uid,
                  static_cast<int>(err));
    }
  }
  err = controller_->ExecuteBatchInviteUsers();
  if (err != SDKERR_SUCCESS) {
    *error = "ExecuteBatchInviteUsers failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

void ZoomTalkbackSource::SetBackgroundVolume(float volume) {
  const std::string id = channel_id();
  if (controller_ == nullptr || id.empty()) return;
  const std::wstring id_w = Widen(id);
  controller_->SetChannelBackgroundVolume(id_w.c_str(), volume);
}

std::string ZoomTalkbackSource::channel_id() const {
  std::lock_guard<std::mutex> lock(m_);
  return channel_id_;
}

std::string ZoomTalkbackSource::last_error() const {
  std::lock_guard<std::mutex> lock(m_);
  return last_error_;
}

bool ZoomTalkbackSource::CanSend() {
  // Audio into a channel nobody has joined measures nothing, so the window
  // opens only once both the channel and at least one listener exist.
  return channel_ready_.load() && users_joined_.load() > 0;
}

bool ZoomTalkbackSource::Send(const int16_t* pcm, int samples) {
  const std::string id = channel_id();
  if (id.empty() || controller_ == nullptr) return false;
  const std::wstring id_w = Widen(id);
  const SDKError err = controller_->SendAudioDataToChannel(
      id_w.c_str(), reinterpret_cast<const char*>(pcm),
      static_cast<unsigned int>(samples * static_cast<int>(sizeof(int16_t))),
      kSampleRate, ZoomSDKAudioChannel_Mono);
  if (err != SDKERR_SUCCESS) {
    send_failures_.fetch_add(1);
    last_send_error_.store(static_cast<int>(err));
    return false;
  }
  return true;
}

// --- IMeetingTalkbackCtrlEvent ----------------------------------------------

void ZoomTalkbackSource::onCreateChannelResponse(const zchar_t* channel_id,
                                                 TalkbackError error) {
  const std::string id = Narrow(channel_id);
  std::printf("[talkback] onCreateChannelResponse: %s (%s)\n", id.c_str(),
              ErrorName(error));
  std::lock_guard<std::mutex> lock(m_);
  if (error == TALKBACK_ERROR_OK) {
    channel_id_ = id;
    channel_ready_.store(true);
  } else {
    last_error_ = ErrorName(error);
  }
}

void ZoomTalkbackSource::onDestroyChannelResponse(const zchar_t* channel_id,
                                                  TalkbackError error) {
  std::printf("[talkback] onDestroyChannelResponse: %s (%s)\n",
              Narrow(channel_id).c_str(), ErrorName(error));
}

void ZoomTalkbackSource::onChannelUserJoinResponse(const zchar_t* channel_id,
                                                   unsigned int user_id,
                                                   TalkbackError error) {
  std::printf("[talkback] user %u join -> %s (%s)\n", user_id,
              Narrow(channel_id).c_str(), ErrorName(error));
  if (error == TALKBACK_ERROR_OK) {
    users_joined_.fetch_add(1);
  } else {
    std::lock_guard<std::mutex> lock(m_);
    last_error_ = ErrorName(error);
  }
}

void ZoomTalkbackSource::onChannelUserLeaveResponse(const zchar_t* channel_id,
                                                    unsigned int user_id,
                                                    TalkbackError error) {
  std::printf("[talkback] user %u left %s (%s)\n", user_id,
              Narrow(channel_id).c_str(), ErrorName(error));
  if (error == TALKBACK_ERROR_OK && users_joined_.load() > 0) {
    users_joined_.fetch_sub(1);
  }
}

void ZoomTalkbackSource::onJoinTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client joined a channel (inviter %u)\n",
              inviter_id);
}

void ZoomTalkbackSource::onLeaveTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client left a channel (inviter %u)\n",
              inviter_id);
}

void ZoomTalkbackSource::onInviterAudioLevel(unsigned int, unsigned int) {}

}  // namespace zc
