#include "control_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace zc {
namespace {

void SendAll(SOCKET s, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const int n = send(s, data + off, static_cast<int>(len - off), 0);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

void SendAll(SOCKET s, const std::string& v) { SendAll(s, v.data(), v.size()); }

// Reads until the end of headers plus any body promised by Content-Length.
// Requests here are tiny (a POST with a one-line body); no streaming needed.
bool ReadRequest(SOCKET s, std::string* out) {
  char buf[2048];
  std::string req;
  size_t body_expected = 0, header_end = std::string::npos;
  for (;;) {
    const int n = recv(s, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    req.append(buf, static_cast<size_t>(n));
    if (header_end == std::string::npos) {
      header_end = req.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const auto cl = req.find("Content-Length:");
        if (cl != std::string::npos && cl < header_end) {
          body_expected = std::strtoul(req.c_str() + cl + 15, nullptr, 10);
        }
        header_end += 4;
      }
    }
    if (header_end != std::string::npos &&
        req.size() >= header_end + body_expected) {
      *out = req;
      return true;
    }
    if (req.size() > 64 * 1024) return false;  // not our client
  }
}

}  // namespace

ControlServer::ControlServer(std::string html, ActionFn on_action)
    : html_(std::move(html)), on_action_(std::move(on_action)) {}

ControlServer::~ControlServer() { Stop(); }

bool ControlServer::Start(uint16_t port, std::string* error) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    *error = "socket() failed";
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse),
             sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  // Loopback only. The panel is this machine's front panel; exposing an
  // unauthenticated talk key to the LAN would be a real product decision, not
  // a default.
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(s, 8) != 0) {
    closesocket(s);
    *error = "could not listen on 127.0.0.1:" + std::to_string(port) +
             " (port in use?)";
    return false;
  }

  listen_socket_ = static_cast<uintptr_t>(s);
  port_ = port;
  running_.store(true);
  accept_thread_ = std::thread(&ControlServer::AcceptLoop, this);
  return true;
}

void ControlServer::Stop() {
  if (!running_.exchange(false)) return;
  closesocket(static_cast<SOCKET>(listen_socket_));
  if (accept_thread_.joinable()) accept_thread_.join();
  for (std::thread& t : client_threads_) {
    if (t.joinable()) t.join();
  }
  client_threads_.clear();
}

void ControlServer::PublishState(const std::string& json) {
  std::lock_guard<std::mutex> lock(state_m_);
  state_json_ = json;
}

void ControlServer::AcceptLoop() {
  while (running_.load()) {
    const SOCKET c = accept(static_cast<SOCKET>(listen_socket_), nullptr, nullptr);
    if (c == INVALID_SOCKET) break;  // listen socket closed by Stop()
    client_threads_.emplace_back(&ControlServer::ServeClient, this,
                                 static_cast<uintptr_t>(c));
  }
}

void ControlServer::ServeClient(uintptr_t socket) {
  const SOCKET s = static_cast<SOCKET>(socket);
  std::string req;
  if (!ReadRequest(s, &req)) {
    closesocket(s);
    return;
  }

  if (req.rfind("GET /events", 0) == 0) {
    SendAll(s,
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
    // Push the latest snapshot every 100 ms until the client goes away or the
    // server stops. Latest-wins by construction: a slow tab misses frames,
    // never queues them.
    while (running_.load()) {
      std::string snap;
      {
        std::lock_guard<std::mutex> lock(state_m_);
        snap = state_json_;
      }
      if (!snap.empty()) {
        std::string frame = "data: " + snap + "\n\n";
        const int n = send(s, frame.data(), static_cast<int>(frame.size()), 0);
        if (n <= 0) break;
      }
      Sleep(100);
    }
  } else if (req.rfind("POST /act", 0) == 0) {
    const auto body_at = req.find("\r\n\r\n");
    std::string body =
        body_at != std::string::npos ? req.substr(body_at + 4) : "";
    // Body is "verb arg", e.g. "talk on", "gain -6".
    const auto sp = body.find(' ');
    const std::string verb = sp == std::string::npos ? body : body.substr(0, sp);
    const std::string arg = sp == std::string::npos ? "" : body.substr(sp + 1);
    if (on_action_ && !verb.empty()) on_action_(verb, arg);
    SendAll(s, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
  } else if (req.rfind("GET /oauth/callback", 0) == 0) {
    // The sign-in return leg. Query string ends at the first space of the
    // request line ("GET /oauth/callback?a=b HTTP/1.1").
    std::string query;
    const size_t q = req.find('?');
    const size_t sp = req.find(' ', 4);
    if (q != std::string::npos && sp != std::string::npos && q < sp) {
      query = req.substr(q + 1, sp - q - 1);
    }
    const std::string verdict = on_oauth_ ? on_oauth_(query) : "no handler";
    const bool ok = verdict.empty();
    std::string page =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>ZComms</title><style>body{font-family:system-ui;"
        "background:#1b1d21;color:#e8e4da;display:grid;place-items:center;"
        "min-height:100vh;margin:0}main{text-align:center}p{color:#8f948e}"
        "</style></head><body><main><h1>" +
        std::string(ok ? "Signed in" : "Sign-in failed") + "</h1><p>" +
        (ok ? "You can close this tab and return to the ZComms panel."
            : verdict) +
        "</p></main></body></html>";
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                  page.size());
    SendAll(s, hdr, std::strlen(hdr));
    SendAll(s, page);
  } else if (req.rfind("GET / ", 0) == 0 || req.rfind("GET / HTTP", 0) == 0) {
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                  html_.size());
    SendAll(s, hdr, std::strlen(hdr));
    SendAll(s, html_);
  } else {
    SendAll(s, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  }
  closesocket(s);
}

}  // namespace zc
