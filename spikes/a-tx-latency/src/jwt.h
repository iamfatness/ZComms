// Meeting SDK auth JWT.
//
// auth_service_interface.h documents the exact payload the SDK expects:
// appKey, iat, exp, tokenExp, signed HS256 with the SDK secret. Generated
// locally here because this is a spike on one machine.
//
// Production must not do this. Plan §3.6 requires a broker endpoint so the
// secret never reaches an end user's machine at all; a shipped binary that can
// mint its own tokens has shipped the secret. This function exists so the
// harness can use either credential type, not as a pattern to carry forward.
#pragma once

#include <string>

namespace zc {

// `valid_seconds` sets both exp and tokenExp. Returns empty on failure.
std::string MakeMeetingSdkJwt(const std::string& sdk_key,
                              const std::string& sdk_secret,
                              int valid_seconds, std::string* error);

}  // namespace zc
