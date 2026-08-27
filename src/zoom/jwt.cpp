#include "jwt.h"

#include <windows.h>

#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace zc {
namespace {

// base64url, unpadded -- what JWT requires, which is not what plain base64
// produces.
std::string Base64Url(const unsigned char* data, size_t len) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back(kAlphabet[n & 63]);
    i += 3;
  }
  if (i + 1 == len) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
  } else if (i + 2 == len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
  }
  return out;
}

std::string Base64Url(const std::string& s) {
  return Base64Url(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

bool HmacSha256(const std::string& key, const std::string& msg,
                std::vector<unsigned char>* out, std::string* error) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                            nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (st != 0) {
    *error = "BCryptOpenAlgorithmProvider failed";
    return false;
  }

  DWORD hash_len = 0, cb = 0;
  st = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                         reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len),
                         &cb, 0);
  if (st != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    *error = "BCryptGetProperty(HASH_LENGTH) failed";
    return false;
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  st = BCryptCreateHash(
      alg, &hash, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
      static_cast<ULONG>(key.size()), 0);
  if (st != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    *error = "BCryptCreateHash failed";
    return false;
  }

  st = BCryptHashData(hash,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())),
                      static_cast<ULONG>(msg.size()), 0);
  if (st == 0) {
    out->resize(hash_len);
    st = BCryptFinishHash(hash, out->data(), hash_len, 0);
  }

  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  if (st != 0) {
    *error = "BCryptHashData/FinishHash failed";
    return false;
  }
  return true;
}

}  // namespace

std::string MakeMeetingSdkJwt(const std::string& sdk_key,
                              const std::string& sdk_secret, int valid_seconds,
                              std::string* error) {
  if (sdk_key.empty() || sdk_secret.empty()) {
    *error = "sdk_key and sdk_secret are both required to mint a JWT";
    return "";
  }

  // Wall-clock on purpose, and the only place in the harness that uses it: iat
  // and exp are absolute times Zoom's servers validate against, so a
  // monotonic clock would be meaningless here. Nothing in the measurement
  // touches this.
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const long long iat = static_cast<long long>(now);
  const long long exp = iat + valid_seconds;

  const std::string header = R"({"alg":"HS256","typ":"JWT"})";
  char payload[512];
  std::snprintf(payload, sizeof(payload),
                R"({"appKey":"%s","iat":%lld,"exp":%lld,"tokenExp":%lld})",
                sdk_key.c_str(), iat, exp, exp);

  const std::string signing_input =
      Base64Url(header) + "." + Base64Url(std::string(payload));

  std::vector<unsigned char> sig;
  if (!HmacSha256(sdk_secret, signing_input, &sig, error)) return "";

  return signing_input + "." + Base64Url(sig.data(), sig.size());
}

}  // namespace zc
