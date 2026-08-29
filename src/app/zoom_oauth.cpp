// See zoom_oauth.h. The wire contract mirrors CoreVideo's ZoomOAuthService /
// the OBS plugin's ZoomOAuthManager, which are the living reference for the
// broker's shapes:
//   GET  <broker>/oauth/start?state=..&return_uri=http://127.0.0.1:P/oauth/callback
//   -> browser -> Zoom consent -> broker callback ->
//   GET  http://127.0.0.1:P/oauth/callback?state=..&broker_token=..
//   POST <broker>/oauth/redeem   {"broker_token": ..}   -> access/refresh
//   POST <broker>/oauth/refresh  {"refresh_token": ..}  -> rotated pair
//   POST <broker>/oauth/sdk-jwt  {"access_token": ..}   -> {"sdk_jwt": ..}
//   GET  https://api.zoom.us/v2/users/me/token?type=zak (Bearer) -> {"token": ..}

#include "zoom_oauth.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdio>
#include <ctime>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace zc {
namespace {

constexpr wchar_t kBrokerHost[] = L"corevideo.iamfatness.us";
constexpr wchar_t kZoomApiHost[] = L"api.zoom.us";

int64_t NowSeconds() { return static_cast<int64_t>(std::time(nullptr)); }

std::string RandomBase64Url(size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((bytes * 4) / 3 + 4);
  unsigned int acc = 0;
  int bits = 0;
  for (unsigned char b : buf) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(kAlphabet[(acc >> bits) & 0x3f]);
    }
  }
  if (bits > 0) out.push_back(kAlphabet[(acc << (6 - bits)) & 0x3f]);
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

// One HTTPS request, response body out. Small, synchronous, WinHTTP -- every
// call here is a sub-second control-plane exchange, never media.
bool HttpsRequest(const wchar_t* host, const wchar_t* verb,
                  const std::wstring& path, const std::string& body,
                  const wchar_t* content_type, const std::string& bearer,
                  int* status_out, std::string* body_out, std::string* error) {
  HINTERNET session = WinHttpOpen(L"ZComms/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0);
  if (!session) {
    *error = "WinHttpOpen failed";
    return false;
  }
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = nullptr;
  if (conn) {
    req = WinHttpOpenRequest(conn, verb, path.c_str(), nullptr,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
  }
  if (req) {
    std::wstring headers;
    if (content_type && *content_type) {
      headers += std::wstring(L"Content-Type: ") + content_type + L"\r\n";
    }
    if (!bearer.empty()) {
      headers += L"Authorization: Bearer " + Widen(bearer) + L"\r\n";
    }
    headers += L"Accept: application/json\r\n";
    if (WinHttpSendRequest(req, headers.c_str(), (DWORD)headers.size(),
                           body.empty() ? WINHTTP_NO_REQUEST_DATA
                                        : (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
      DWORD status = 0, len = sizeof(status);
      WinHttpQueryHeaders(req,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                          WINHTTP_NO_HEADER_INDEX);
      *status_out = (int)status;
      body_out->clear();
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        body_out->append(chunk.data(), read);
      }
      ok = true;
    } else {
      *error = "https request to host failed (network?) code " +
               std::to_string(GetLastError());
    }
  } else {
    *error = "WinHttp request setup failed";
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}

// The responses here are flat JSON objects whose values are URL-safe token
// strings -- a targeted field scan is enough, and it keeps a JSON library
// out of the build. Not a general parser on purpose.
std::string JsonField(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t at = json.find(needle);
  if (at == std::string::npos) return "";
  at = json.find(':', at + needle.size());
  if (at == std::string::npos) return "";
  ++at;
  while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
  if (at >= json.size() || json[at] != '"') return "";
  ++at;
  std::string out;
  while (at < json.size() && json[at] != '"') {
    if (json[at] == '\\' && at + 1 < json.size()) ++at;
    out.push_back(json[at++]);
  }
  return out;
}

int64_t JsonNumberField(const std::string& json, const std::string& key,
                        int64_t fallback) {
  const std::string needle = "\"" + key + "\"";
  size_t at = json.find(needle);
  if (at == std::string::npos) return fallback;
  at = json.find(':', at + needle.size());
  if (at == std::string::npos) return fallback;
  return std::strtoll(json.c_str() + at + 1, nullptr, 10);
}

std::string BrokerErrorMessage(const std::string& body) {
  std::string m = JsonField(body, "reason");
  if (m.empty()) m = JsonField(body, "message");
  if (m.empty()) m = JsonField(body, "error");
  if (m.empty()) m = body.substr(0, 200);
  return m;
}

std::string QueryValue(const std::string& query, const std::string& key) {
  size_t at = 0;
  while (at < query.size()) {
    size_t amp = query.find('&', at);
    if (amp == std::string::npos) amp = query.size();
    const size_t eq = query.find('=', at);
    if (eq != std::string::npos && eq < amp &&
        query.compare(at, eq - at, key) == 0) {
      std::string raw = query.substr(eq + 1, amp - eq - 1);
      // Percent-decode; token values are URL-safe so this covers the field.
      std::string out;
      for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
          out.push_back((char)std::strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16));
          i += 2;
        } else if (raw[i] == '+') {
          out.push_back(' ');
        } else {
          out.push_back(raw[i]);
        }
      }
      return out;
    }
    at = amp + 1;
  }
  return "";
}

std::wstring TokenFilePath() {
  wchar_t base[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) return L"";
  std::wstring dir = std::wstring(base) + L"\\ZComms";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\zoom-tokens.bin";
}

}  // namespace

ZoomOAuth::ZoomOAuth(int panel_port) : panel_port_(panel_port) {}

bool ZoomOAuth::LoadTokens(Tokens* t) {
  const std::wstring path = TokenFilePath();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(f, nullptr);
  std::vector<BYTE> blob(size);
  DWORD read = 0;
  const bool got = ReadFile(f, blob.data(), size, &read, nullptr) && read == size;
  CloseHandle(f);
  if (!got || blob.empty()) return false;

  DATA_BLOB in{(DWORD)blob.size(), blob.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return false;
  }
  const std::string json((char*)out.pbData, out.cbData);
  LocalFree(out.pbData);
  t->access = JsonField(json, "access");
  t->refresh = JsonField(json, "refresh");
  t->expires_at = JsonNumberField(json, "expires_at", 0);
  return !t->access.empty() || !t->refresh.empty();
}

bool ZoomOAuth::SaveTokens(const Tokens& t) {
  std::string json = "{\"access\":\"" + t.access + "\",\"refresh\":\"" +
                     t.refresh + "\",\"expires_at\":" +
                     std::to_string(t.expires_at) + "}";
  DATA_BLOB in{(DWORD)json.size(), (BYTE*)json.data()}, out{};
  if (!CryptProtectData(&in, L"ZComms Zoom tokens", nullptr, nullptr, nullptr,
                        0, &out)) {
    return false;
  }
  const std::wstring path = TokenFilePath();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = false;
  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = WriteFile(f, out.pbData, out.cbData, &written, nullptr) &&
         written == out.cbData;
    CloseHandle(f);
  }
  LocalFree(out.pbData);
  return ok;
}

void ZoomOAuth::ClearTokens() { DeleteFileW(TokenFilePath().c_str()); }

bool ZoomOAuth::signed_in() {
  std::lock_guard<std::mutex> lock(m_);
  Tokens t;
  return LoadTokens(&t);
}

std::string ZoomOAuth::BeginSignIn() {
  std::lock_guard<std::mutex> lock(m_);
  pending_state_ = RandomBase64Url(32);
  return "https://corevideo.iamfatness.us/oauth/start?state=" + pending_state_ +
         "&return_uri=http%3A%2F%2F127.0.0.1%3A" + std::to_string(panel_port_) +
         "%2Foauth%2Fcallback";
}

bool ZoomOAuth::HandleCallback(const std::string& query, std::string* error) {
  std::lock_guard<std::mutex> lock(m_);
  const std::string oauth_error = QueryValue(query, "error");
  if (!oauth_error.empty()) {
    *error = "Zoom sign-in failed: " + oauth_error;
    return false;
  }
  const std::string state = QueryValue(query, "state");
  const std::string broker_token = QueryValue(query, "broker_token");
  if (pending_state_.empty()) {
    *error = "no sign-in is in progress -- start again from the panel";
    return false;
  }
  if (state != pending_state_) {
    *error = "sign-in state mismatch -- start again from the panel";
    return false;
  }
  if (broker_token.empty()) {
    *error = "the callback carried no broker token";
    return false;
  }
  pending_state_.clear();

  int status = 0;
  std::string body, net_err;
  if (!HttpsRequest(kBrokerHost, L"POST", L"/oauth/redeem",
                    "{\"broker_token\":\"" + broker_token + "\"}",
                    L"application/json", "", &status, &body, &net_err)) {
    *error = "could not reach the sign-in broker: " + net_err;
    return false;
  }
  if (status < 200 || status >= 300) {
    *error = "sign-in redeem failed: " + BrokerErrorMessage(body);
    return false;
  }
  Tokens t;
  t.access = JsonField(body, "access_token");
  t.refresh = JsonField(body, "refresh_token");
  t.expires_at = NowSeconds() + JsonNumberField(body, "expires_in", 3600) - 60;
  if (t.access.empty()) {
    *error = "the broker returned no access token";
    return false;
  }
  if (!SaveTokens(t)) {
    *error = "could not store the sign-in";
    return false;
  }
  std::printf("[oauth] signed in with Zoom\n");
  return true;
}

bool ZoomOAuth::RefreshLocked(Tokens* t, std::string* error) {
  if (t->refresh.empty()) {
    *error = "Zoom sign-in expired -- sign in again";
    return false;
  }
  int status = 0;
  std::string body, net_err;
  if (!HttpsRequest(kBrokerHost, L"POST", L"/oauth/refresh",
                    "{\"refresh_token\":\"" + t->refresh + "\"}",
                    L"application/json", "", &status, &body, &net_err)) {
    *error = "could not reach the sign-in broker: " + net_err;
    return false;
  }
  if (status < 200 || status >= 300) {
    // Zoom rotates refresh tokens and revokes superseded generations; a
    // retry can never recover from invalid_grant, so clear and re-ask
    // (CoreVideo's handle_dead_refresh_grant, same reasoning).
    if (body.find("invalid_grant") != std::string::npos) {
      ClearTokens();
      *error = "Zoom sign-in was revoked -- sign in again";
      return false;
    }
    *error = "token refresh failed: " + BrokerErrorMessage(body);
    return false;
  }
  t->access = JsonField(body, "access_token");
  const std::string new_refresh = JsonField(body, "refresh_token");
  if (!new_refresh.empty()) t->refresh = new_refresh;
  t->expires_at = NowSeconds() + JsonNumberField(body, "expires_in", 3600) - 60;
  if (t->access.empty()) {
    *error = "token refresh returned no access token";
    return false;
  }
  SaveTokens(*t);
  return true;
}

bool ZoomOAuth::EnsureJoinCredentials(std::string* sdk_jwt, std::string* zak,
                                      std::string* error) {
  std::lock_guard<std::mutex> lock(m_);
  Tokens t;
  if (!LoadTokens(&t)) {
    *error = "not signed in";
    return false;
  }
  if (t.expires_at <= NowSeconds() && !RefreshLocked(&t, error)) return false;

  int status = 0;
  std::string body, net_err;
  if (!HttpsRequest(kBrokerHost, L"POST", L"/oauth/sdk-jwt",
                    "{\"access_token\":\"" + t.access + "\"}",
                    L"application/json", "", &status, &body, &net_err)) {
    *error = "could not reach the sign-in broker: " + net_err;
    return false;
  }
  if (status < 200 || status >= 300) {
    // The access token may have been revoked out from under its expiry;
    // one refresh-and-retry covers it before giving up.
    if (!RefreshLocked(&t, error)) return false;
    if (!HttpsRequest(kBrokerHost, L"POST", L"/oauth/sdk-jwt",
                      "{\"access_token\":\"" + t.access + "\"}",
                      L"application/json", "", &status, &body, &net_err) ||
        status < 200 || status >= 300) {
      *error = "Meeting SDK auth broker failed: " + BrokerErrorMessage(body);
      return false;
    }
  }
  *sdk_jwt = JsonField(body, "sdk_jwt");
  if (sdk_jwt->empty()) {
    *error = "the broker returned no SDK JWT";
    return false;
  }

  if (!HttpsRequest(kZoomApiHost, L"GET", L"/v2/users/me/token?type=zak", "",
                    nullptr, t.access, &status, &body, &net_err)) {
    *error = "could not reach the Zoom API: " + net_err;
    return false;
  }
  if (status < 200 || status >= 300) {
    *error = "could not fetch the Zoom ZAK: " + BrokerErrorMessage(body);
    return false;
  }
  *zak = JsonField(body, "token");
  if (zak->empty()) {
    *error = "Zoom did not return a ZAK token";
    return false;
  }
  return true;
}

void ZoomOAuth::SignOut() {
  std::lock_guard<std::mutex> lock(m_);
  pending_state_.clear();
  ClearTokens();
  std::printf("[oauth] signed out\n");
}

}  // namespace zc
