// Chat signaling wire format (pure, no SDK includes, no windows.h).
//
// Zoom chat is ZComms's data side-channel: structured cues between desks,
// human-readable assignment notices to talent on stock Zoom clients, and a
// fallback cue path when talkback is unavailable. A signaling message is a
// chat message whose content starts with the literal prefix ~ZC1~ followed
// by one flat, single-line JSON object. The version is the 1 in the prefix:
// an unknown prefix version is IGNORED, never an error -- old desks stay
// quiet about traffic from newer ones.
#pragma once

#include <string>

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
// false = not ours (no prefix, wrong version, unknown kind) -- never an
// error path, chat is full of human text.
bool DecodeSignal(const std::string& content, SignalMsg* out);
bool IsSignal(const std::string& content);  // prefix check only

// The human-facing assignment notice for talent on stock Zoom clients:
// plain text, deliberately NOT protocol.
std::string AssignNoticeText(const std::string& person,
                             const std::string& channel_name);

}  // namespace zc
