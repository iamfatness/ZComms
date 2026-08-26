// The talkback transport: Zoom's own private-channel audio path.
//
// This is the transport the product actually wants for "talk to the
// panelists": IMeetingTalkbackController creates private channels inside the
// client's meeting (max 16, max 10 listeners each), the invited participants
// hear the channel audio over their ordinary Zoom connection with the meeting
// duckable underneath, and nobody else hears a word. It is the API behind
// ZoomISO's talkback feature, present since Meeting SDK 7.0.
//
// Strategically this replaces the plan §1 premise that "channel routing does
// not exist inside a Zoom meeting" -- it does now, and it collapses the
// crew-meeting-per-channel model into channels inside the one meeting the
// client is already running.
//
// What is unproven and what this spike therefore establishes:
//   - entitlement: the CoreVideo talkback design (2026-08-24) infers
//     host/co-host role and Enhanced Media on the host account, but neither
//     is documented. TALKBACK_ERROR_NOPERMISSION is the failure to watch.
//   - latency: SendAudioDataToChannel -> a plain client's ears has no
//     published figure. That is the number the kill criterion now applies to.
//
// As a FrameSink, CanSend() opens once the channel exists and at least one
// invited user has confirmed joining -- audio sent into a channel nobody is
// in measures nothing.
#pragma once

// See mic_source.h: the SDK headers depend on windows.h having been included.
// clang-format off
#include <windows.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
#include "tx_pacer.h"

namespace zc {

class ZoomTalkbackSource : public ZOOM_SDK_NAMESPACE::IMeetingTalkbackCtrlEvent,
                           public FrameSink {
 public:
  explicit ZoomTalkbackSource(
      ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller);

  // Creates one channel. Asynchronous; completion arrives via
  // onCreateChannelResponse and is reflected in channel_ready().
  bool CreateChannel(std::string* error);

  // Invites the given users to the channel. Also asynchronous; each user's
  // join lands in onChannelUserJoinResponse.
  bool InviteUsers(const std::vector<unsigned int>& user_ids,
                   std::string* error);

  // Ducks the main meeting under the talkback voice for channel members.
  void SetBackgroundVolume(float volume);

  bool channel_ready() const { return channel_ready_.load(); }
  int users_joined() const { return users_joined_.load(); }
  bool meeting_supports_talkback() const;
  std::string channel_id() const;
  // Non-empty once any callback reported a TalkbackError; kept for the
  // operator-facing diagnosis.
  std::string last_error() const;

  // FrameSink
  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;

  uint64_t send_failures() const { return send_failures_.load(); }
  int last_send_error() const { return last_send_error_.load(); }

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
  void onInviterAudioLevel(unsigned int inviter_id,
                           unsigned int audio_level) override;

  static const char* ErrorName(TalkbackError e);

 private:
  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller_;

  mutable std::mutex m_;
  std::string channel_id_;
  std::string last_error_;

  std::atomic<bool> channel_ready_{false};
  std::atomic<int> users_joined_{0};
  std::atomic<uint64_t> send_failures_{0};
  std::atomic<int> last_send_error_{0};
};

}  // namespace zc
