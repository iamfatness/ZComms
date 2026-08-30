#include "signal_protocol.h"

#include <cstdio>
#include <cstdlib>

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

// Hand scanner for the flat one-line objects this protocol emits -- the
// repo's dependency-free house rule. Finds "key": and reads one value;
// this is not a general JSON parser and does not pretend to be.
size_t FindValue(const std::string& j, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const size_t at = j.find(needle);
  return at == std::string::npos ? std::string::npos : at + needle.size();
}

bool GetString(const std::string& j, const std::string& key, std::string* v) {
  size_t at = FindValue(j, key);
  if (at == std::string::npos || at >= j.size() || j[at] != '"') return false;
  ++at;
  std::string out;
  while (at < j.size() && j[at] != '"') {
    if (j[at] == '\\' && at + 1 < j.size()) ++at;
    out.push_back(j[at++]);
  }
  if (at >= j.size()) return false;  // unterminated
  *v = std::move(out);
  return true;
}

bool GetInt(const std::string& j, const std::string& key, int* v) {
  const size_t at = FindValue(j, key);
  if (at == std::string::npos) return false;
  *v = std::atoi(j.c_str() + at);
  return true;
}

bool GetBool(const std::string& j, const std::string& key, bool* v) {
  const size_t at = FindValue(j, key);
  if (at == std::string::npos) return false;
  if (j.compare(at, 4, "true") == 0) {
    *v = true;
    return true;
  }
  if (j.compare(at, 5, "false") == 0) {
    *v = false;
    return true;
  }
  return false;
}

}  // namespace

bool IsSignal(const std::string& content) {
  return content.rfind(kPrefix, 0) == 0;
}

std::string EncodeSignal(const SignalMsg& m) {
  const char* t = m.kind == SignalKind::kCue        ? "cue"
                  : m.kind == SignalKind::kAssign   ? "assign"
                  : m.kind == SignalKind::kFallback ? "fallback"
                                                    : "hello";
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "%s{\"t\":\"%s\",\"slot\":%d,\"on\":%s,\"ch\":\"%s\",\"from\":\"%s\"}",
      kPrefix, t, m.slot, m.on ? "true" : "false",
      JsonEscape(m.channel_name).c_str(), JsonEscape(m.from).c_str());
  return buf;
}

bool DecodeSignal(const std::string& content, SignalMsg* out) {
  if (!IsSignal(content)) return false;
  const std::string j = content.substr(sizeof(kPrefix) - 1);
  std::string t;
  if (!GetString(j, "t", &t)) return false;
  SignalMsg m;
  if (t == "cue") m.kind = SignalKind::kCue;
  else if (t == "assign") m.kind = SignalKind::kAssign;
  else if (t == "fallback") m.kind = SignalKind::kFallback;
  else if (t == "hello") m.kind = SignalKind::kHello;
  else return false;  // unknown kind from a newer desk: ignore, never error
  GetInt(j, "slot", &m.slot);
  GetBool(j, "on", &m.on);
  GetString(j, "ch", &m.channel_name);
  GetString(j, "from", &m.from);
  *out = std::move(m);
  return true;
}

std::string AssignNoticeText(const std::string& person,
                             const std::string& channel_name) {
  // Legend-plate voice, stock-Zoom-client audience: say what happened and
  // what to expect, nothing else.
  return person + ", you are on talkback " + channel_name +
         " -- the director can speak to you privately there.";
}

}  // namespace zc
