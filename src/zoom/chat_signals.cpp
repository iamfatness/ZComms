#include "chat_signals.h"

#include <cstdio>

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
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      w.data(), n);
  return w;
}

}  // namespace

void ChatSignals::Attach(IMeetingChatController* controller,
                         OnSignalFn on_signal) {
  controller_ = controller;
  on_signal_ = std::move(on_signal);
  if (controller_ != nullptr) controller_->SetEvent(this);
}

void ChatSignals::SendSignalTo(unsigned int user_id, const SignalMsg& m) {
  outbox_.Push({user_id, EncodeSignal(m)});
}

void ChatSignals::SendAssignNotice(unsigned int user_id,
                                   const std::string& person,
                                   const std::string& channel_name) {
  outbox_.Push({user_id, AssignNoticeText(person, channel_name)});
}

void ChatSignals::BroadcastFallback(bool active) {
  SignalMsg m;
  m.kind = SignalKind::kFallback;
  m.on = active;
  outbox_.Push({0, EncodeSignal(m)});
}

void ChatSignals::Tick(int64_t now_ms) {
  OutboundChat m;
  while (controller_ != nullptr && outbox_.PopReady(now_ms, &m)) {
    IChatMsgInfoBuilder* builder = controller_->GetChatMessageBuilder();
    if (builder == nullptr) return;
    const std::wstring content_w = Widen(m.content);
    builder->SetContent(content_w.c_str());
    if (m.receiver_user_id != 0) {
      builder->SetReceiver(m.receiver_user_id);
      builder->SetMessageType(SDKChatMessageType_To_Individual);
    } else {
      builder->SetReceiver(0);
      builder->SetMessageType(SDKChatMessageType_To_All);
    }
    IChatMsgInfo* msg = builder->Build();
    if (msg == nullptr) return;
    const SDKError err = controller_->SendChatMsgTo(msg);
    if (err != SDKERR_SUCCESS) {
      std::printf("[chat] send failed: %d\n", static_cast<int>(err));
    }
  }
}

void ChatSignals::onChatMsgNotification(IChatMsgInfo* chatMsg,
                                        const zchar_t* /*content: documented
                                                         "currently invalid" */) {
  if (chatMsg == nullptr) return;
  const std::string content = Narrow(chatMsg->GetContent());
  SignalMsg m;
  if (!DecodeSignal(content, &m)) return;  // human chat: not ours, not touched
  if (on_signal_) on_signal_(m, chatMsg->GetSenderUserId());
  // Best-effort hiding of our own echoed signaling traffic. The SDK offers
  // no way to suppress rendering on a receiver's stock client; a private
  // message is already invisible to third parties.
  const zchar_t* id = chatMsg->GetMessageID();
  if (id != nullptr && controller_ != nullptr &&
      controller_->IsChatMessageCanBeDeleted(id)) {
    controller_->DeleteChatMessage(id);
  }
}

void ChatSignals::onChatStatusChangedNotification(ChatStatus* status) {
  if (status == nullptr) return;
  bool can = true;
  if (!status->is_webinar_meeting) {
    can = status->ut.normal_meeting_status.can_chat;
  }
  if (can != can_chat_) {
    can_chat_ = can;
    // One line per transition, not per tick: the fallback path just went
    // down (or came back), and the operator must know.
    std::printf("[chat] %s\n", can ? "chat re-enabled -- signaling path up"
                                   : "host disabled chat -- signaling path "
                                     "down");
  }
}

}  // namespace zc
