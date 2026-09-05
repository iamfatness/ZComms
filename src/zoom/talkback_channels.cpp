#include "talkback_channels.h"

#include <cstdio>

#include "audio_defs.h"

namespace zc {
namespace {

// r.code is what every caller below branches on; r.raw is appended so the
// ops line still names the exact platform code a real incident hit (e.g.
// Windows SDKERR_NO_PERMISSION == 12, "you need to be host";
// SDKERR_INVALID_PARAMETER == 3, "that person is on the web client") --
// both collapse to TalkbackCall::Failed and were unrecoverable from `code`
// alone.
std::string Describe(const TalkbackResult& r) {
  return std::string(TalkbackCallName(r.code)) + " (sdk " +
         std::to_string(r.raw) + ")";
}

}  // namespace

TalkbackChannels::TalkbackChannels(TalkbackSdk* sdk) : sdk_(sdk) {
  if (sdk_ != nullptr) sdk_->SetEvents(this);
}

bool TalkbackChannels::meeting_supports_talkback() const {
  return sdk_ != nullptr && sdk_->MeetingSupportsTalkback();
}

bool TalkbackChannels::CreateChannels(int count, std::string* error) {
  if (sdk_ == nullptr) {
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
  const TalkbackResult r =
      sdk_->CreateChannels(static_cast<unsigned int>(count));
  if (r != TalkbackCall::Ok) {
    *error = "CreateChannel failed: " + Describe(r);
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
    send_ids_[i] = channels_[i].id;
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
  const TalkbackResult r = sdk_->InviteUsers(id, {user_id});
  if (r != TalkbackCall::Ok) {
    *error = "invite failed: " + Describe(r);
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
  const TalkbackResult r = sdk_->InviteUsers(id, user_ids);
  if (r != TalkbackCall::Ok) {
    *error = Describe(r);
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
  const TalkbackResult r = sdk_->RemoveUsers(id, {user_id});
  if (r != TalkbackCall::Ok) {
    *error = "remove failed: " + Describe(r);
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

bool TalkbackChannels::SetChannelVolume(int slot, float volume) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (slot < 0 || slot >= static_cast<int>(channels_.size()) ||
        !channels_[static_cast<size_t>(slot)].ready) {
      return false;
    }
    id = channels_[static_cast<size_t>(slot)].id;
  }
  return sdk_->SetChannelBackgroundVolume(id, volume) == TalkbackCall::Ok;
}

bool TalkbackChannels::SendToSlot(int slot, const int16_t* pcm, int samples) {
  if (slot < 0 || slot >= kMaxChannels || sdk_ == nullptr) return false;
  if (((ready_mask_.load() >> slot) & 1u) == 0) return false;
  std::lock_guard<std::mutex> lock(send_m_);
  // Mono only -- law #5, same as SendToKeyed below.
  const TalkbackResult r =
      sdk_->SendAudio(send_ids_[static_cast<size_t>(slot)], pcm, samples);
  if (r != TalkbackCall::Ok) {
    send_failures_.fetch_add(1);
    return false;
  }
  channel_sends_.fetch_add(1);
  sent_mask_.fetch_or(1u << slot);
  return true;
}

int TalkbackChannels::SendToKeyed(const int16_t* pcm, int samples) {
  const uint32_t live = key_mask_.load() & ready_mask_.load();
  if (live == 0 || sdk_ == nullptr) return 0;
  int sent = 0;
  std::lock_guard<std::mutex> lock(send_m_);
  for (int i = 0; i < kMaxChannels; ++i) {
    if (((live >> i) & 1u) == 0) continue;
    // Mono is a LAW, not a preference: a stereo send returns success and
    // delivers NOTHING audible (CoreVideo, live). A stereo source must be
    // downmixed before this boundary; the seam hardcodes mono so the law
    // cannot be broken from here.
    const TalkbackResult r =
        sdk_->SendAudio(send_ids_[static_cast<size_t>(i)], pcm, samples);
    if (r == TalkbackCall::Ok) {
      ++sent;
      channel_sends_.fetch_add(1);
      sent_mask_.fetch_or(1u << i);
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

// --- TalkbackSdkEvents -------------------------------------------------------

void TalkbackChannels::OnCreateChannelResponse(const std::string& channel_id,
                                               TalkbackEvent error) {
  const std::string& id = channel_id;
  std::printf("[talkback] channel response: %s (%s)\n", id.c_str(),
              TalkbackEventName(error));
  std::lock_guard<std::mutex> lock(m_);
  if (error != TalkbackEvent::Ok) {
    last_error_ = TalkbackEventName(error);
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

void TalkbackChannels::OnDestroyChannelResponse(const std::string& channel_id,
                                                TalkbackEvent error) {
  std::printf("[talkback] destroyed: %s (%s)\n", channel_id.c_str(),
              TalkbackEventName(error));
}

void TalkbackChannels::OnChannelUserJoinResponse(const std::string& channel_id,
                                                 unsigned int user_id,
                                                 TalkbackEvent error) {
  const std::string& id = channel_id;
  std::printf("[talkback] user %u -> %s (%s)\n", user_id, id.c_str(),
              TalkbackEventName(error));
  std::lock_guard<std::mutex> lock(m_);
  const int slot = SlotForId(id);
  if (slot < 0) return;
  ChannelState& c = channels_[static_cast<size_t>(slot)];
  if (error == TalkbackEvent::Ok) {
    c.members.insert(user_id);
    c.listeners = static_cast<int>(c.members.size());
  } else {
    last_error_ = TalkbackEventName(error);
  }
}

void TalkbackChannels::OnChannelUserLeaveResponse(const std::string& channel_id,
                                                  unsigned int user_id,
                                                  TalkbackEvent error) {
  const std::string& id = channel_id;
  std::printf("[talkback] user %u left %s (%s)\n", user_id, id.c_str(),
              TalkbackEventName(error));
  std::lock_guard<std::mutex> lock(m_);
  const int slot = SlotForId(id);
  if (slot < 0) return;
  if (error == TalkbackEvent::Ok) {
    ChannelState& c = channels_[static_cast<size_t>(slot)];
    c.members.erase(user_id);
    c.listeners = static_cast<int>(c.members.size());
  }
}

void TalkbackChannels::OnJoinTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client joined a channel (inviter %u)\n",
              inviter_id);
}

void TalkbackChannels::OnLeaveTalkbackChannel(unsigned int inviter_id) {
  std::printf("[talkback] this client left a channel (inviter %u)\n",
              inviter_id);
}

}  // namespace zc
