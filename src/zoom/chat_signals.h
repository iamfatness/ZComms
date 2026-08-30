// Chat signaling SDK glue. See signal_protocol.h for the wire format and
// signal_outbox.h for why every send is paced -- this class is only the
// hands: build+send outbound on Tick (pump thread), decode inbound in
// onChatMsgNotification, track permission truth from chat-status events.
//
// Hiding honesty: the SDK has no invisible/data-only chat. A private
// message is already invisible to everyone but the addressee, and this
// class best-effort deletes its OWN signaling messages after delivery
// (IsChatMessageCanBeDeleted + DeleteChatMessage); a receiver's stock Zoom
// client may still render inbound protocol lines -- stated in the ops docs,
// not papered over.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <functional>
#include <set>
#include <string>

#include "meeting_service_components/meeting_chat_interface.h"
#include "signal_outbox.h"
#include "signal_protocol.h"

namespace zc {

class ChatSignals : public ZOOM_SDK_NAMESPACE::IMeetingChatCtrlEvent {
 public:
  using OnSignalFn =
      std::function<void(const SignalMsg&, unsigned int sender_id)>;

  void Attach(ZOOM_SDK_NAMESPACE::IMeetingChatController* controller,
              OnSignalFn on_signal);

  void SendSignalTo(unsigned int user_id, const SignalMsg& m);  // queued
  void SendAssignNotice(unsigned int user_id, const std::string& person,
                        const std::string& channel_name);  // human text
  // Fallback goes ONLY to known peer desks (senders of decoded signals):
  // a To_All protocol line rendered in every attendee's chat (stock clients
  // show inbound ~ZC1~ lines), and an audience that cannot decode it gains
  // nothing from seeing it. No peers known = nothing sent.
  void SignalFallback(bool active);

  // Pump thread: drains the outbox through the builder, paced.
  void Tick(int64_t now_ms);

  bool chat_allowed() const { return can_chat_; }
  uint64_t dropped() const { return outbox_.dropped(); }

  // IMeetingChatCtrlEvent
  void onChatMsgNotification(ZOOM_SDK_NAMESPACE::IChatMsgInfo* chatMsg,
                             const zchar_t* content) override;
  void onChatStatusChangedNotification(
      ZOOM_SDK_NAMESPACE::ChatStatus* status) override;
  void onChatMsgDeleteNotification(
      const zchar_t*, ZOOM_SDK_NAMESPACE::SDKChatMessageDeleteType) override {}
  void onChatMessageEditNotification(
      ZOOM_SDK_NAMESPACE::IChatMsgInfo*) override {}
  void onShareMeetingChatStatusChanged(bool) override {}
  void onFileSendStart(ZOOM_SDK_NAMESPACE::ISDKFileSender*) override {}
  void onFileReceived(ZOOM_SDK_NAMESPACE::ISDKFileReceiver*) override {}
  void onFileTransferProgress(
      ZOOM_SDK_NAMESPACE::SDKFileTransferInfo*) override {}

 private:
  ZOOM_SDK_NAMESPACE::IMeetingChatController* controller_ = nullptr;
  OnSignalFn on_signal_;
  SignalOutbox outbox_;
  bool can_chat_ = true;  // optimistic until a status event says otherwise
  // Peer desks: anyone whose chat decoded as ours. User ids are
  // meeting-scoped, so this set dies with the session (it lives in a
  // session-scoped ChatSignals) -- never persist it.
  std::set<unsigned int> peers_;
};

}  // namespace zc
