# Chat as a Data Side-Channel: Structured Cues, Assignment Notices, Fallback Signaling

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Use `IMeetingChatController` as ZComms's data side-channel: structured JSON cues between desks, human-readable channel-assignment notices to talent not running zcomms.exe, tally/cue state distribution, and a fallback signaling path when talkback channels are unavailable.

## Architecture

A pure, SDK-free protocol module (`signal_protocol`) defines the wire format — a recognizable `~ZC1~` prefix followed by compact JSON — and a rate-limited outbox (`SignalOutbox`) that paces sends the way `TalkbackChannels` paces invites, because Zoom rate-limits back-to-back SDK calls (`SDKERR_TOO_FREQUENT_CALL`, code 18, hit live on talkback invites 2026-08-29). A thin SDK glue class (`ChatSignals`) implements `IMeetingChatCtrlEvent`, sends via `IChatMsgInfoBuilder`/`SendChatMsgTo`, decodes inbound messages, and hides recognized signaling traffic from human chat where the SDK allows (self-`DeleteChatMessage` after delivery; private messages already limit visibility to the addressee). The app wires it into the session loop next to `TalkbackChannels`.

## Tech Stack

- C++17, MSVC, CMake (existing build).
- Zoom Meeting SDK 7.1.5 (vendored, gitignored `third_party/zoom-sdk/`): `meeting_service_components/meeting_chat_interface.h`.
- zctest harness (`tests/audio/test_util.h`, `ZC_CHECK` / `ZC_TEST`), single test exe `zcomms_audio_tests`.
- No JSON library: the messages are flat key/value; encode/decode by hand like the rest of the repo (dependency-free is a house rule).

## Spec

This document doubles as the spec.

### Requirements

1. Every signaling message is a chat message whose content starts with the literal prefix `~ZC1~` followed by a single-line JSON object. Version is the `1` in the prefix; an unknown prefix version is ignored, never an error.
2. Message kinds (field `t`): `cue` (tally/cue state: channel slot + on/off), `assign` (you-are-on-channel notice), `fallback` (talkback unavailable, chat is now the cue path), `hello` (desk announces itself so peers know who speaks the protocol).
3. Assignment notices to talent NOT running zcomms.exe are plain human text (no prefix), sent as a private message (`SDKChatMessageType_To_Individual`) so only that person sees it in their stock Zoom client.
4. Structured messages between desks are also private (`SDKChatMessageType_To_Individual`) whenever a specific addressee exists; `SDKChatMessageType_To_All` only for `fallback` broadcast.
5. Recognized inbound signaling messages are processed then hidden from the operator's panel chat surface unconditionally; on the Zoom-client side the sender additionally calls `IsChatMessageCanBeDeleted` + `DeleteChatMessage` on its own signaling messages (best effort — the SDK offers no way to suppress rendering on a receiver's stock client, and a private message is already invisible to everyone else; state this honestly in ops docs).
6. Sends are paced: at most one chat send per 300 ms, queued FIFO, queue depth capped at 64 with drop-oldest and a counted drop stat — never a blocking send on the SDK pump thread.
7. Everything decodable is unit-tested with no SDK present; the SDK glue is compile-verified plus a live checklist.

### SDK reality (verified against vendored headers)

- `IMeetingChatController::GetChatMessageBuilder()` returns `IChatMsgInfoBuilder*`; chain `SetContent(const zchar_t*)`, `SetReceiver(unsigned int)`, `SetMessageType(SDKChatMessageType)`, then `Build()`, then `IMeetingChatController::SendChatMsgTo(IChatMsgInfo*)`.
- Inbound: `IMeetingChatCtrlEvent::onChatMsgNotification(IChatMsgInfo* chatMsg, const zchar_t* content)` — the `content` json parameter is documented "currently invalid"; use `IChatMsgInfo::GetContent()`.
- `IChatMsgInfo::GetSenderUserId()`, `GetMessageID()`, `GetChatMessageType()` exist. Receiver id 0 = to-all.
- Hiding: only `DeleteChatMessage(const zchar_t* msgID)` guarded by `IsChatMessageCanBeDeleted(msgID)` exists. There is **no** SDK interface for invisible/data-only chat (nothing like a data channel) in the vendored 7.1.5 headers — that is why requirement 5 is best-effort by design.
- Permission truth arrives via `onChatStatusChangedNotification(ChatStatus*)` / `GetChatStatus()` — a host can restrict chat (`SDK_CHAT_PRIVILEGE_HOST` etc.), which kills the fallback path; surface that as an ops line, do not retry silently.

## Global Constraints

- Build: `cmake -S . -B build` then `cmake --build build --config Release`. Tests: `build\Release\zcomms_audio_tests.exe` (also `ctest --test-dir build -C Release`).
- Zoom SDK headers require `windows.h` included first; pure modules must include **no** SDK headers so they compile into `zcomms_audio_tests` without `sdk.lib`.
- `SDKERR_TOO_FREQUENT_CALL` (18) is real on back-to-back SDK calls; pace all chat sends (requirement 6).
- Zoom user ids are meeting-scoped and recycled: keys in any durable structure are display names, never ids (plan §5; `Roster` is the only id-touching layer).
- Never run work inline on the audio path; chat lives entirely on the SDK pump thread.
- Update `CLAUDE.md` in the same change as any substantive work.
- Commits on a feature branch off `main`, conventional messages.

## Tasks

### Task 1: `signal_protocol` — wire format encode/decode (pure)

**Files**
- Create: `src/zoom/signal_protocol.h`
- Create: `src/zoom/signal_protocol.cpp`
- Test: `tests/zoom/test_signal_protocol.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Produces:
  ```cpp
  namespace zc {
  enum class SignalKind { kCue, kAssign, kFallback, kHello };
  struct SignalMsg {
    SignalKind kind = SignalKind::kHello;
    int slot = -1;             // cue/assign: channel slot 0..15
    bool on = false;           // cue: keyed state
    std::string channel_name;  // assign: operator-facing label ("CH 3")
    std::string from;          // hello/assign: sender display name
  };
  std::string EncodeSignal(const SignalMsg& m);
  bool DecodeSignal(const std::string& content, SignalMsg* out);  // false = not ours
  bool IsSignal(const std::string& content);                      // prefix check only
  std::string AssignNoticeText(const std::string& person,
                               const std::string& channel_name);  // human text, no prefix
  }
  ```
- Consumes: nothing (pure strings).

**Steps**

- [ ] Write the failing test `tests/zoom/test_signal_protocol.cpp`:
  ```cpp
  #include <string>
  #include "signal_protocol.h"
  #include "test_util.h"

  void TestSignalProtocol() {
    ZC_TEST("signal: cue round-trips");
    zc::SignalMsg m;
    m.kind = zc::SignalKind::kCue;
    m.slot = 3;
    m.on = true;
    const std::string wire = zc::EncodeSignal(m);
    ZC_CHECK(wire.rfind("~ZC1~", 0) == 0);
    zc::SignalMsg back;
    ZC_CHECK(zc::DecodeSignal(wire, &back));
    ZC_CHECK(back.kind == zc::SignalKind::kCue);
    ZC_CHECK(back.slot == 3);
    ZC_CHECK(back.on == true);

    ZC_TEST("signal: assign carries channel name and sender");
    zc::SignalMsg a;
    a.kind = zc::SignalKind::kAssign;
    a.slot = 2;
    a.channel_name = "CH 3";
    a.from = "Desk A";
    zc::SignalMsg aback;
    ZC_CHECK(zc::DecodeSignal(zc::EncodeSignal(a), &aback));
    ZC_CHECK(aback.channel_name == "CH 3");
    ZC_CHECK(aback.from == "Desk A");

    ZC_TEST("signal: non-signal text is rejected, not an error");
    zc::SignalMsg junk;
    ZC_CHECK(!zc::DecodeSignal("hello everyone", &junk));
    ZC_CHECK(!zc::DecodeSignal("~ZC2~{\"t\":\"cue\"}", &junk));  // future version: ignore
    ZC_CHECK(!zc::IsSignal("~zc1~{}"));                          // case-sensitive prefix

    ZC_TEST("signal: quotes and backslashes in names survive");
    zc::SignalMsg q;
    q.kind = zc::SignalKind::kHello;
    q.from = "A \"B\" \\ C";
    zc::SignalMsg qback;
    ZC_CHECK(zc::DecodeSignal(zc::EncodeSignal(q), &qback));
    ZC_CHECK(qback.from == "A \"B\" \\ C");

    ZC_TEST("signal: assign notice is plain text, not protocol");
    const std::string notice = zc::AssignNoticeText("Pat", "CH 3");
    ZC_CHECK(!zc::IsSignal(notice));
    ZC_CHECK(notice.find("CH 3") != std::string::npos);
  }
  ```
- [ ] Register it: add `void TestSignalProtocol();` at the bottom of `tests/audio/test_util.h` (after `void TestAec();`) and call `TestSignalProtocol();` after `TestAec();` in `tests/audio/test_main.cpp`.
- [ ] Wire the build in `CMakeLists.txt` — extend the test target:
  ```cmake
  add_executable(zcomms_audio_tests
    tests/audio/test_main.cpp
    tests/audio/test_envelope.cpp
    tests/audio/test_limiter.cpp
    tests/audio/test_frame_accumulator.cpp
    tests/audio/test_sample_ring.cpp
    tests/audio/test_frame_ring.cpp
    tests/audio/test_aec.cpp
    tests/zoom/test_signal_protocol.cpp
    src/zoom/signal_protocol.cpp
  )
  target_include_directories(zcomms_audio_tests PRIVATE tests/audio src/zoom)
  ```
  (`src/zoom/signal_protocol.cpp` is listed directly because `zcomms_zoom` only exists when the SDK is present; the protocol must test on a clean clone.)
- [ ] Run `cmake -S . -B build` then `cmake --build build --config Release --target zcomms_audio_tests`; expect a link/compile failure (module absent) — that is the failing state.
- [ ] Implement `src/zoom/signal_protocol.h` with the interface above (no SDK includes, no `windows.h`), and `src/zoom/signal_protocol.cpp`:
  ```cpp
  #include "signal_protocol.h"

  #include <cstdio>

  namespace zc {
  namespace {
  constexpr char kPrefix[] = "~ZC1~";

  std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      if (c == '"' || c == '\\') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  }

  // Extracts "key":"value" (string) from a flat one-line JSON object,
  // honouring backslash escapes. Returns false if the key is absent.
  bool GetString(const std::string& j, const std::string& key, std::string* v);
  // Extracts "key":<int> or "key":true/false.
  bool GetInt(const std::string& j, const std::string& key, int* v);
  bool GetBool(const std::string& j, const std::string& key, bool* v);
  }  // namespace

  bool IsSignal(const std::string& content) {
    return content.rfind(kPrefix, 0) == 0;
  }

  std::string EncodeSignal(const SignalMsg& m) {
    const char* t = m.kind == SignalKind::kCue      ? "cue"
                    : m.kind == SignalKind::kAssign ? "assign"
                    : m.kind == SignalKind::kFallback ? "fallback"
                                                      : "hello";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s{\"t\":\"%s\",\"slot\":%d,\"on\":%s,\"ch\":\"%s\",\"from\":\"%s\"}",
                  kPrefix, t, m.slot, m.on ? "true" : "false",
                  JsonEscape(m.channel_name).c_str(), JsonEscape(m.from).c_str());
    return buf;
  }
  // DecodeSignal: check prefix, parse the five fields with the helpers,
  // map unknown "t" to false (ignore, never error).
  }  // namespace zc
  ```
  Implement the three `Get*` helpers with a small hand scanner (find `"key":`, then read a JSON string with escape handling, an integer, or `true`/`false`).
- [ ] Rebuild and run `build\Release\zcomms_audio_tests.exe`; all checks pass, existing 22 stay green.
- [ ] Commit: `git add src/zoom/signal_protocol.h src/zoom/signal_protocol.cpp tests/zoom/test_signal_protocol.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(chat): signal_protocol wire format for chat signaling (~ZC1~ + flat JSON)"`

### Task 2: `SignalOutbox` — paced send queue (pure)

**Files**
- Create: `src/zoom/signal_outbox.h`
- Create: `src/zoom/signal_outbox.cpp`
- Test: `tests/zoom/test_signal_outbox.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Produces:
  ```cpp
  namespace zc {
  struct OutboundChat {
    unsigned int receiver_user_id = 0;  // 0 = to all
    std::string content;
  };
  class SignalOutbox {
   public:
    static constexpr int kMinSendGapMs = 300;
    static constexpr size_t kMaxQueued = 64;
    void Push(const OutboundChat& msg);          // drop-oldest past kMaxQueued
    // Returns true and fills *out when now_ms is past the pacing gate.
    bool PopReady(int64_t now_ms, OutboundChat* out);
    uint64_t dropped() const;
    size_t pending() const;
  };
  }
  ```
- Consumes: caller-supplied monotonic milliseconds (clock injection — same pattern as `TxPacer`).

**Steps**

- [ ] Write the failing test `tests/zoom/test_signal_outbox.cpp`:
  ```cpp
  #include "signal_outbox.h"
  #include "test_util.h"

  void TestSignalOutbox() {
    ZC_TEST("outbox: paces to one send per 300 ms");
    zc::SignalOutbox ob;
    ob.Push({1, "a"});
    ob.Push({2, "b"});
    zc::OutboundChat m;
    ZC_CHECK(ob.PopReady(1000, &m));
    ZC_CHECK(m.content == "a");
    ZC_CHECK(!ob.PopReady(1200, &m));   // 200 ms later: gated
    ZC_CHECK(ob.PopReady(1300, &m));    // 300 ms later: released
    ZC_CHECK(m.content == "b");
    ZC_CHECK(!ob.PopReady(9999, &m));   // empty

    ZC_TEST("outbox: drop-oldest past 64, counted");
    zc::SignalOutbox full;
    for (int i = 0; i < 70; ++i) full.Push({0, std::to_string(i)});
    ZC_CHECK(full.pending() == zc::SignalOutbox::kMaxQueued);
    ZC_CHECK(full.dropped() == 6);
    zc::OutboundChat first;
    ZC_CHECK(full.PopReady(0, &first));
    ZC_CHECK(first.content == "6");  // 0..5 were dropped oldest-first
  }
  ```
- [ ] Register `TestSignalOutbox` in `tests/audio/test_util.h` and `tests/audio/test_main.cpp`; add `tests/zoom/test_signal_outbox.cpp` and `src/zoom/signal_outbox.cpp` to `zcomms_audio_tests` in `CMakeLists.txt`.
- [ ] Build; confirm the compile failure, then implement `SignalOutbox` with a `std::deque<OutboundChat>`, `int64_t last_send_ms_ = INT64_MIN`, `uint64_t dropped_ = 0`, and a `std::mutex` (Push may later be called from the panel action thread; PopReady runs on the pump thread).
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/zoom/signal_outbox.h src/zoom/signal_outbox.cpp tests/zoom/test_signal_outbox.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(chat): SignalOutbox paced chat send queue (300 ms gap, drop-oldest)"`

### Task 3: `ChatSignals` — the SDK glue

**Files**
- Create: `src/zoom/chat_signals.h`
- Create: `src/zoom/chat_signals.cpp`
- Modify: `CMakeLists.txt` (add `src/zoom/chat_signals.cpp` to the `zcomms_zoom` target's source list)
- Modify: `src/zoom/zoom_client.h`, `src/zoom/zoom_client.cpp` (add `GetChatController()` accessor)

**Interfaces**
- Consumes: `ZOOM_SDK_NAMESPACE::IMeetingChatController` (from `IMeetingService::GetMeetingChatController()`), `SignalMsg`/`EncodeSignal`/`DecodeSignal`, `SignalOutbox`.
- Produces:
  ```cpp
  namespace zc {
  class ChatSignals : public ZOOM_SDK_NAMESPACE::IMeetingChatCtrlEvent {
   public:
    using OnSignalFn = std::function<void(const SignalMsg&, unsigned int sender_id)>;
    void Attach(ZOOM_SDK_NAMESPACE::IMeetingChatController* controller,
                OnSignalFn on_signal);
    void SendSignalTo(unsigned int user_id, const SignalMsg& m);   // queued
    void SendAssignNotice(unsigned int user_id, const std::string& person,
                          const std::string& channel_name);        // human text, queued
    void BroadcastFallback(bool active);                           // to-all
    void Tick(int64_t now_ms);  // pump thread: drains SignalOutbox via the builder
    bool chat_allowed() const;  // last ChatStatus said we can send
    // IMeetingChatCtrlEvent
    void onChatMsgNotification(ZOOM_SDK_NAMESPACE::IChatMsgInfo* chatMsg,
                               const zchar_t* content) override;
    void onChatStatusChangedNotification(ZOOM_SDK_NAMESPACE::ChatStatus* status) override;
    void onChatMsgDeleteNotification(const zchar_t* msgID,
                                     ZOOM_SDK_NAMESPACE::SDKChatMessageDeleteType deleteBy) override {}
    void onChatMessageEditNotification(ZOOM_SDK_NAMESPACE::IChatMsgInfo*) override {}
    void onShareMeetingChatStatusChanged(bool) override {}
    void onFileSendStart(ZOOM_SDK_NAMESPACE::ISDKFileSender*) override {}
    void onFileReceived(ZOOM_SDK_NAMESPACE::ISDKFileReceiver*) override {}
    void onFileTransferProgress(ZOOM_SDK_NAMESPACE::SDKFileTransferInfo*) override {}
  };
  }
  ```
- `ZoomClient` gains, mirroring `GetTalkbackController()`:
  ```cpp
  ZOOM_SDK_NAMESPACE::IMeetingChatController* GetChatController();
  // impl: return meeting_ ? meeting_->GetMeetingChatController() : nullptr;
  ```

**Steps**

- [ ] Add the `GetChatController()` accessor to `zoom_client.{h,cpp}` exactly as above.
- [ ] Implement `chat_signals.{h,cpp}`. `Tick` pops from the outbox and sends:
  ```cpp
  void ChatSignals::Tick(int64_t now_ms) {
    OutboundChat m;
    while (controller_ != nullptr && outbox_.PopReady(now_ms, &m)) {
      auto* builder = controller_->GetChatMessageBuilder();
      if (builder == nullptr) return;
      const std::wstring content_w = Widen(m.content);  // same helper family as zoom_client.cpp
      builder->SetContent(content_w.c_str());
      if (m.receiver_user_id != 0) {
        builder->SetReceiver(m.receiver_user_id);
        builder->SetMessageType(ZOOM_SDK_NAMESPACE::SDKChatMessageType_To_Individual);
      } else {
        builder->SetReceiver(0);
        builder->SetMessageType(ZOOM_SDK_NAMESPACE::SDKChatMessageType_To_All);
      }
      ZOOM_SDK_NAMESPACE::IChatMsgInfo* msg = builder->Build();
      if (msg == nullptr) return;
      const ZOOM_SDK_NAMESPACE::SDKError err = controller_->SendChatMsgTo(msg);
      if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        std::printf("[chat] send failed: %d\n", static_cast<int>(err));
      }
    }
  }
  ```
  `onChatMsgNotification`: narrow `GetContent()`, `DecodeSignal`; on success invoke `on_signal_` with `GetSenderUserId()`, then hide: capture `GetMessageID()` and, if the sender was self (signal echoed back) and `controller_->IsChatMessageCanBeDeleted(id)`, call `controller_->DeleteChatMessage(id)`. Non-signal messages are left alone.
  `onChatStatusChangedNotification`: record `can_chat` from the `NormalMeetingChatStatus` arm of the union when `!is_webinar_meeting`; when sending becomes disallowed, print one `[chat] host disabled chat -- signaling path down` line (once per transition, not per tick).
- [ ] Add `src/zoom/chat_signals.cpp` to the `zcomms_zoom` sources in `CMakeLists.txt`.
- [ ] Build the full app: `cmake --build build --config Release --target zcomms`. Compile-clean is this task's gate (the class is exercised live in Task 4).
- [ ] Commit: `git add src/zoom/chat_signals.h src/zoom/chat_signals.cpp src/zoom/zoom_client.h src/zoom/zoom_client.cpp CMakeLists.txt && git commit -m "feat(chat): ChatSignals SDK glue -- builder sends, decode, status, self-delete hiding"`

### Task 4: app wiring — cues, assignment notices, fallback

**Files**
- Modify: `src/app/main.cpp`
- Modify: `src/app/ui_html.h` (ops ticker already renders `log_op` lines; no new panel controls in this slice)
- Modify: `CLAUDE.md`

**Interfaces**
- Consumes: `ChatSignals`, `TalkbackChannels::Snapshot()` / `key_mask()`, `Roster::others()` (`RosterMember::name`, `user_id`, `supports_talkback`), the existing `/act` verb dispatch in `main.cpp`.
- Produces: new `/act` verbs — `cue <slot> on|off` (send a `kCue` signal to every member of that slot's channel) and `notify <slot>` (send `AssignNoticeText` to every member of the slot's channel as a private human-readable chat message).

**Steps**

- [ ] In the meeting-session lambda in `src/app/main.cpp`, after `TalkbackChannels` is constructed: construct `ChatSignals`, `chat.Attach(client.GetChatController(), on_signal)` where `on_signal` does `log_op("cue from " + sender_name + ": CH " + ...)` (resolve the sender id through `Roster` at callback time; never store the id).
- [ ] Call `chat.Tick(now_ms)` once per main-loop iteration (pump thread), next to the existing housekeeping timer.
- [ ] Add the `cue` and `notify` verb branches to the `/act` dispatch, translating slot → channel members via `TalkbackChannels::Snapshot()` and members → names via `Roster`.
- [ ] Auto-assignment notices: where the direct-talk model auto-lands a participant on their own channel (the `auto_assigned` per-session state), queue `SendAssignNotice(user_id, name, channel_name)` — this is the path that reaches talent on stock Zoom clients.
- [ ] Fallback: where channel creation fails permanently (the existing three-round partial-grant give-up) or `IsMeetingSupportTalkBack()` is false, call `chat.BroadcastFallback(true)` once and `log_op("talkback unavailable -- cues via chat")`.
- [ ] Live verification (needs a meeting; follow CLAUDE.md's test-meeting rules — meeting started from the authorizing account, no waiting room): (a) `POST /act "notify 0"` and confirm the private notice renders only in the target client's chat; (b) `POST /act "cue 0 on"` between two zcomms desks and confirm the receiving desk logs the cue and the message does not remain in the sender's chat history; (c) have the host set chat to host-only and confirm the one-line ops warning.
- [ ] Update `CLAUDE.md` (a short "Chat signaling" subsection under the talkback sections: prefix, pacing, the no-hide-on-stock-clients truth).
- [ ] Run the full suite once more: `build\Release\zcomms_audio_tests.exe` — all green.
- [ ] Commit: `git add src/app/main.cpp src/app/ui_html.h CLAUDE.md && git commit -m "feat(chat): cue/notify verbs, auto-assign notices, talkback-down fallback via chat"`

## Self-review checklist (fix inline before PR)

- [ ] No `TBD`, no "similar to", every step has real code or an exact command.
- [ ] `SignalMsg` field types match between test, header, and sketches (`int slot`, `bool on`, `std::string` names).
- [ ] Pure modules include no SDK header and no `windows.h`.
- [ ] Verify with `build\Release\zcomms_audio_tests.exe` output before claiming green.
