// The local control surface (plan §6.4), first transport: HTTP on localhost.
//
// One tiny server, four routes:
//   GET  /               -> the panel UI (embedded HTML, no files, no CDN)
//   GET  /events         -> Server-Sent Events; a state snapshot every ~100 ms
//   POST /act            -> an action: "talk on|off", "latch on|off",
//                           "gain <db>", "sidetone on|off"
//   GET  /oauth/callback -> the Zoom sign-in return leg (RFC 8252 loopback);
//                           the query string goes to the oauth handler, the
//                           response is a "return to ZComms" page
//
// SSE rather than WebSocket on purpose: the down-channel is a state stream
// (latest-wins, loss-tolerant) and the up-channel is small discrete actions,
// which plain POSTs carry fine on localhost. That halves the protocol surface
// and keeps this dependency-free -- no frameworks, no handshake crypto.
//
// This is the same seam a Companion/Stream Deck module will drive later; the
// browser panel is just its first client. Binds 127.0.0.1 only: this is an
// instrument's front panel, not a network service.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zc {

class ControlServer {
 public:
  // `on_action` is called on a server thread with (verb, argument); the
  // implementation must be thread-safe and cheap -- set an atomic, return.
  using ActionFn = std::function<void(const std::string&, const std::string&)>;

  // `on_oauth` receives the /oauth/callback query string and returns the
  // operator-facing status line rendered on the browser page ("" = success).
  // Runs on a server client thread; it may block for one short HTTPS round
  // trip (the broker redeem) -- the SSE stream rides its own thread.
  using OAuthFn = std::function<std::string(const std::string&)>;

  ControlServer(std::string html, ActionFn on_action);
  ~ControlServer();

  void SetOAuthHandler(OAuthFn fn) { on_oauth_ = std::move(fn); }

  // Binds and listens on 127.0.0.1:port. Returns false with `error` if the
  // port is taken.
  bool Start(uint16_t port, std::string* error);
  void Stop();

  // Replaces the snapshot pushed to every /events client. Called from the
  // app's main loop; a JSON object string.
  void PublishState(const std::string& json);

  uint16_t port() const { return port_; }

 private:
  void AcceptLoop();
  void ServeClient(uintptr_t socket);

  std::string html_;
  ActionFn on_action_;
  OAuthFn on_oauth_;
  std::atomic<bool> running_{false};
  uintptr_t listen_socket_ = ~0ull;
  uint16_t port_ = 0;
  std::thread accept_thread_;
  std::vector<std::thread> client_threads_;
  std::mutex state_m_;
  std::string state_json_;
};

}  // namespace zc
