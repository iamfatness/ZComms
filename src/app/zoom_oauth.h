// Zoom sign-in, the CoreVideo way.
//
// ZComms never joins anonymously: the operator signs in with Zoom once and
// every join happens as that account. The heavy lifting lives in the
// CoreVideo OAuth broker (a Cloudflare Worker that holds the app credentials,
// runs PKCE against Zoom, and mints Meeting SDK JWTs) -- this client only
// opens the browser, catches the loopback callback, redeems the broker token,
// and keeps the token pair fresh. Discovered the hard way why this matters:
// under bare public-app-key auth Zoom refuses to let the app join meetings
// on accounts that never authorized it (error 504, 2026-08-29).
//
// Tokens at rest are DPAPI-protected (%APPDATA%\ZComms\zoom-tokens.bin),
// the same rule CoreVideo's shell follows: never write a secret plaintext.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace zc {

class ZoomOAuth {
 public:
  // panel_port names the loopback callback the broker redirects back to:
  // http://127.0.0.1:<port>/oauth/callback -- served by the panel's own
  // ControlServer, so sign-in needs no protocol registration and no helper.
  explicit ZoomOAuth(int panel_port);

  bool signed_in();

  // Mints the state nonce and returns the browser URL for the broker's
  // /oauth/start. Open it in the SYSTEM browser (their Zoom session lives
  // there), never in the panel's WebView.
  std::string BeginSignIn();

  // The panel server hands the /oauth/callback query string here (runs on
  // the server's client thread; the redeem is one short HTTPS round trip).
  // Returns false with an operator-facing reason on any mismatch/failure.
  bool HandleCallback(const std::string& query, std::string* error);

  // Refreshes if needed, then returns everything a signed-in join wants:
  // the Meeting SDK JWT (broker-minted) and the user's ZAK. On a revoked
  // refresh token the stored credentials are cleared -- retrying can never
  // recover (Zoom rotates refresh tokens); the fix is a fresh sign-in.
  bool EnsureJoinCredentials(std::string* sdk_jwt, std::string* zak,
                             std::string* error);

  void SignOut();

 private:
  struct Tokens {
    std::string access;
    std::string refresh;
    int64_t expires_at = 0;  // unix seconds, minus a safety minute
  };

  bool LoadTokens(Tokens* t);
  bool SaveTokens(const Tokens& t);
  void ClearTokens();
  bool RefreshLocked(Tokens* t, std::string* error);

  std::mutex m_;
  int panel_port_;
  std::string pending_state_;
};

}  // namespace zc
