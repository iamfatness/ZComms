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
  void BroadcastFallback(bool active);                     // to-all

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
};

}  // namespace zc
