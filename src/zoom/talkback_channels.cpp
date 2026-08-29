#include "talkback_channels.h"

#include <cstdio>

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

}  // namespace

const char* TalkbackChannels::ErrorName(TalkbackError e) {
  switch (e) {
    case TALKBACK_ERROR_OK: return "OK";
    case TALKBACK_ERROR_NOPERMISSION: return "NO_PERMISSION (need host/co-host)";
    case TALKBACK_ERROR_ALREADY_EXIST: return "ALREADY_EXIST";
    case TALKBACK_ERROR_COUNT_OVERFLOW: return "COUNT_OVERFLOW (max 16)";
    case TALKBACK_ERROR_NOT_EXIST: return "NOT_EXIST";
    case TALKBACK_ERROR_REJECTED: return "REJECTED";
    case TALKBACK_ERROR_TIMEOUT: return "TIMEOUT";
    default: return "UNKNOWN";
  }
}

TalkbackChannels::TalkbackChannels(IMeetingTalkbackController* controller)
    : controller_(controller) {
  if (controller_ != nullptr) controller_->SetEvent(this);
}

bool TalkbackChannels::meeting_supports_talkback() const {
  return controller_ != nullptr && controller_->IsMeetingSupportTalkBack();
}

bool TalkbackChannels::CreateChannels(int count, std::string* error) {
  if (controller_ == nullptr) {
    *error = "no talkback controller";
    return false;
  }
  if (count < 1 || count > kMaxChannels) {
    *error = "channel count must be 1..16";
    return false;
  }
  want_ = count;
  {
    std::lock_guard<std::mutex> lock(m_);
    channels_.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      channels_[static_cast<size_t>(i)].name = "CH " + std::to_string(i + 1);
    }
  }
  // One call for the lot. CreateChannel is known to be rate-limited (found
  // live by the CoreVideo talkback work), so N channels are requested as one
  // batch rather than N calls.
  const SDKError err = controller_->CreateChannel(static_cast<unsigned int>(count));
  if (err != SDKERR_SUCCESS) {
    *error = "CreateChannel failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

int TalkbackChannels::channels_ready() const {
  std::lock_guard<std::mutex> lock(m_);
  int n = 0;
  for (const ChannelState& c : channels_) {
    if (c.ready) ++n;
  }
  return n;
}

int TalkbackChannels::SlotForId(const std::string& id) const {
  for (size_t i = 0; i < channels_.size(); ++i) {
    if (channels_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

void TalkbackChannels::RefreshSendIds() {
  // m_ is held by the caller. Rebuild the pacer's snapshot.
  std::lock_guard<std::mutex> lock(send_m_);
  uint32_t mask = 0;
  for (size_t i = 0; i < channels_.size() && i < kMaxChannels; ++i) {
    send_ids_[i] = Widen(channels_[i].id);
    if (channels_[i].ready) mask |= 1u << i;
  }
  ready_mask_.store(mask);
}

bool TalkbackChannels::Invite(int slot, unsigned int user_id,
                              std::string* error) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (slot < 0 || slot >= static_cast<int>(channels_.size()) ||
        !channels_[static_cast<size_t>(slot)].ready) {
      *error = "channel not ready";
      return false;
    }
    if (channels_[static_cast<size_t>(slot)].members.size() >=
        static_cast<size_t>(kMaxMembers)) {
      *error = "channel is full (10 members)";
      return false;
    }
    id = channels_[static_cast<size_t>(slot)].id;
  }
  const std::wstring idw = Widen(id);
  SDKError err = controller_->BeginBatchInviteUsers(idw.c_str());
  if (err == SDKERR_SUCCESS) err = controller_->AddUserToInvite(user_id);
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchInviteUsers();
  if (err != SDKERR_SUCCESS) {
    *error = "invite failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

bool TalkbackChannels::InviteMany(int slot,
                                  const std::vector<unsigned int>& user_ids,
                                  std::string* error) {
  if (user_ids.empty()) return true;
  std::string id;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (slot < 0 || slot >= static_cast<int>(channels_.size()) ||
        !channels_[static_cast<size_t>(slot)].ready) {
      *error = "channel not ready";
      return false;
    }
    if (channels_[static_cast<size_t>(slot)].members.size() + user_ids.size() >
        static_cast<size_t>(kMaxMembers)) {
      *error = "channel would exceed 10 members";
      return false;
    }
    id = channels_[static_cast<size_t>(slot)].id;
  }
  const std::wstring idw = Widen(id);
  SDKError err = controller_->BeginBatchInviteUsers(idw.c_str());
  for (size_t i = 0; i < user_ids.size() && err == SDKERR_SUCCESS; ++i) {
    err = controller_->AddUserToInvite(user_ids[i]);
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchInviteUsers();
  if (err != SDKERR_SUCCESS) {
    *error = "code " + std::to_string(static_cast<int>(err)) +
             (static_cast<int>(err) == 18 ? " (rate limited)" : "");
    return false;
  }
  return true;
}

bool TalkbackChannels::Remove(int slot, unsigned int user_id,
                              std::string* error) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (slot < 0 || slot >= static_cast<int>(channels_.size())) {
      *error = "no such channel";
      return false;
    }
    id = channels_[static_cast<size_t>(slot)].id;
  }
  const std::wstring idw = Widen(id);
  SDKError err = controller_->BeginBatchRemoveUsers(idw.c_str());
  if (err == SDKERR_SUCCESS) err = controller_->AddUserToRemove(user_id);
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchRemoveUsers();
  if (err != SDKERR_SUCCESS) {
    *error = "remove failed: " + std::to_string(static_cast<int>(err));
    return false;
  }
  return true;
}

void TalkbackChannels::SetKey(int slot, bool down) {
  if (slot < 0 || slot >= kMaxChannels) return;
  uint32_t mask = key_mask_.load();
  for (;;) {
    const uint32_t next =
        down ? (mask | (1u << slot)) : (mask & ~(1u << slot));
    if (key_mask_.compare_exchange_weak(mask, next)) break;
  }
}

void TalkbackChannels::SetBackgroundVolumeAll(float volume) {
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lock(m_);
    for (const ChannelState& c : channels_) {
      if (c.ready) ids.push_back(c.id);
    }
  }
  for (const std::string& id : ids) {
    const std::wstring idw = Widen(id);
    controller_->SetChannelBackgroundVolume(idw.c_str(), volume);
  }
}

int TalkbackChannels::SendToKeyed(const int16_t* pcm, int samples) {
  const uint32_t live = key_mask_.load() & ready_mask_.load();
  if (live == 0 || controller_ == nullptr) return 0;
  int sent = 0;
  std::lock_guard<std::mutex> lock(send_m_);
  for (int i = 0; i < kMaxChannels; ++i) {
    if (((live >> i) & 1u) == 0) continue;
    const SDKError err = controller_->SendAudioDataToChannel(
        send_ids_[static_cast<size_t>(i)].c_str(),
        reinterpret_cast<const char*>(pcm),
        static_cast<unsigned int>(samples * static_cast<int>(sizeof(int16_t))),
        kSampleRate, ZoomSDKAudioChannel_Mono);
    if (err == SDKERR_SUCCESS) {
      ++sent;
    } else {
      send_failures_.fetch_add(1);
    }
  }
  return sent;
}

std::vector<ChannelState> TalkbackChannels::Snapshot() const {
  std::lock_guard<std::mutex> lock(m_);
  return channels_;
}

std::string TalkbackChannels::last_error() const {
  std::lock_guard<std::mutex> lock(m_);
  return last_error_;
}

// --- IMeetingTalkbackCtrlEvent ----------------------------------------------

void TalkbackChannels::onCreateChannelResponse(const zchar_t* channel_id,
                                               TalkbackError error) {
  const std::string id = Narrow(channel_id);
  std::printf("[talkback] channel response: %s (%s)\n", id.c_str(),
              ErrorName(error));
  std::lock_guard<std::mutex> lock(m_);
  if (error != TALKBACK_ERROR_OK) {
    last_error_ = ErrorName(error);
    return;
  }
  // Slot = first empty. Zoom promises nothing about response ordering, so
  // slots are assigned by arrival and named locally.
  for (ChannelState& c : channels_) {
    if (c.id.empty()) {
      c.id = id;
      c.ready = true;
      break;
    }
  }
  RefreshSendIds();
}

void TalkbackChannels::onDestroyChannelResponse(const zchar_t* channel_id,
                                                TalkbackError error) {
  std::printf("[talkback] destroyed: %s (%s)\n", Narrow(channel_id).c_str(),
              ErrorName(error));
}

void TalkbackChannels::onChannelUserJoinResponse(const zchar_t* channel_id,
                                                 unsigned int user_id,
                                                 TalkbackError error) {
  const std::string id = Narrow(channel_id);
  std::printf("[talkback] user %u -> %s (%s)\n", user_id, id.c_str(),
              ErrorName(error));
  std::lock_guard<std::mutex> lock(m_);
  const int slot = SlotForId(id);
  if (slot < 0) return;
  ChannelState& c = channels_[static_cast<size_t>(slot)];
  if (error == TALKBACK_ERROR_OK) {
    c.members.insert(user_id);
    c.listeners = static_cast<int>(c.members.size());
  } else {
    last_error_ = ErrorName(error);
  }
}

void TalkbackChannels::onChannelUserLeaveResponse(const zchar_t* channel_id,
                                                  unsigned int user_id,
                                                  TalkbackError error) {
  const std::string id = Narrow(channel_id);
  std::printf("[talkback] user %u left %s (%s)\n", user_id, id.c_str(),
              ErrorName(error));
  std::lock_guard<std::mutex> lock(m_);
  const int slot = SlotForId(id);
  if (slot < 0) return;
  if (error == TALKBACK_ERROR_OK) {
    ChannelState& c = channels_[static_cast<size_t>(slot)];
    c.members.erase(user_id);
    c.listeners = static_cast<int>(c.members.size());
  }
}

void TalkbackChannels::onJoinTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client joined a channel (inviter %u)\n",
              inviter_id);
}

void TalkbackChannels::onLeaveTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client left a channel (inviter %u)\n",
              inviter_id);
}

}  // namespace zc
