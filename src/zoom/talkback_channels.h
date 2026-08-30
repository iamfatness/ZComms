// N talkback channels and the routing between them.
//
// The single-channel ZoomTalkbackSource proved the transport; this is the
// product shape: up to 16 channels inside the client's meeting (the SDK's
// hard cap), each with its own membership and its own talk key. The routing
// rule is the whole point and is enforced here, in exactly one place: a frame
// goes to a channel if and only if that channel is keyed, and Zoom delivers
// it to that channel's members and nobody else.
//
// Threading: channel lifecycle and membership run on the SDK pump thread.
// SendToKeyed() runs on the TX pacer thread and touches only an atomic key
// mask plus an id snapshot refreshed under a mutex -- the pacer never waits
// on membership work.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "meeting_service_components/meeting_talkback_ctrl_interface.h"

namespace zc {

struct ChannelState {
  std::string id;          // Zoom's channel id; empty until created
  std::string name;        // operator-facing label ("CH 1", renamable later)
  bool ready = false;
  int listeners = 0;
  std::set<unsigned int> members;  // confirmed joins, by user id
};

class TalkbackChannels : public ZOOM_SDK_NAMESPACE::IMeetingTalkbackCtrlEvent {
 public:
  static constexpr int kMaxChannels = 16;  // SDK cap
  static constexpr int kMaxMembers = 10;   // SDK cap per channel

  explicit TalkbackChannels(
      ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller);

  bool meeting_supports_talkback() const;

  // Requests `count` channels. Asynchronous: created channels appear as
  // responses arrive, in arrival order (Zoom does not promise an ordering, so
  // slot index is assigned here, not inferred).
  bool CreateChannels(int count, std::string* error);
  int channels_ready() const;
  int channel_count() const { return want_; }

  // Membership, by slot. Both asynchronous with response callbacks.
  bool Invite(int slot, unsigned int user_id, std::string* error);
  // Everyone missing from a channel in ONE SDK exchange (BeginBatch/Add*N/
  // Execute). Zoom rate-limits back-to-back talkback calls (code 18, hit
  // live 2026-08-29 with a 12-person roster when the healer issued one
  // invite call per person) -- the batch API exists for exactly this.
  bool InviteMany(int slot, const std::vector<unsigned int>& user_ids,
                  std::string* error);
  bool Remove(int slot, unsigned int user_id, std::string* error);

  // Keying. The mask is what SendToKeyed routes by; setting it is wait-free.
  void SetKey(int slot, bool down);
  bool key(int slot) const { return (key_mask_.load() >> slot) & 1u; }
  uint32_t key_mask() const { return key_mask_.load(); }

  // One channel's meeting-audio gain under the talkback voice (0.0-2.0,
  // 1.0 = unity). Zoom ducks channel members BY DEFAULT, so unity must be
  // applied at creation and ducking reserved for while the channel is keyed
  // -- the DuckPlanner in the main loop owns that policy; this is only the
  // SDK call. Returns false on refusal (e.g. code 18) so the planner can
  // back off.
  bool SetChannelVolume(int slot, float volume);
  uint32_t ready_mask() const { return ready_mask_.load(); }

  // From the TX pacer: one frame, fanned out to every keyed, ready channel.
  // Returns the number of channels it was sent to.
  int SendToKeyed(const int16_t* pcm, int samples);

  // Snapshot for UI/state. Pump thread.
  std::vector<ChannelState> Snapshot() const;
  uint64_t send_failures() const { return send_failures_.load(); }
  // Successful channel sends and the channels that have ever taken audio.
  // fails=0 alone cannot distinguish "audio accepted by Zoom" from "nothing
  // was ever sent" -- the 2026-08-29 no-audio hunt stalled on exactly that.
  uint64_t channel_sends() const { return channel_sends_.load(); }
  uint32_t sent_mask() const { return sent_mask_.load(); }
  std::string last_error() const;

  // IMeetingTalkbackCtrlEvent
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

  static const char* ErrorName(TalkbackError e);

 private:
  int SlotForId(const std::string& id) const;  // -1 if unknown
  void RefreshSendIds();

  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller_;
  int want_ = 0;

  mutable std::mutex m_;
  std::vector<ChannelState> channels_;
  std::string last_error_;

  // The pacer's view: wide-string ids per slot, refreshed whenever a channel
  // is created, plus the key mask. Reading a stable snapshot beats taking m_
  // fifty times a second on the audio path.
  std::mutex send_m_;
  std::array<std::wstring, kMaxChannels> send_ids_;
  std::atomic<uint32_t> ready_mask_{0};
  std::atomic<uint32_t> key_mask_{0};
  std::atomic<uint64_t> send_failures_{0};
  std::atomic<uint64_t> channel_sends_{0};
  std::atomic<uint32_t> sent_mask_{0};
};

}  // namespace zc
