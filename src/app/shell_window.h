// ShellWindow: the native window that IS the app.
//
// The panel is served over localhost either way; this hosts it in a WebView2
// window owned by zcomms itself, so the product is one window with its own
// taskbar identity instead of a borrowed browser frame. Runs on its own
// thread with its own message pump -- the main thread pumps the Zoom SDK and
// must never also own a UI message loop.
#pragma once

#include <functional>
#include <string>

namespace zc {

// True when the WebView2 runtime is installed (evergreen on Win 11; absent
// only on unmanaged older machines, where the caller falls back to a browser
// window).
bool ShellWindowAvailable();

// Opens the shell window on a dedicated thread and returns immediately.
// client_w/client_h size the panel area itself -- the frame is added around
// it, so the panel renders at exactly the designed size. on_closed fires on
// the shell thread when the operator closes the window: closing the window
// is quitting the app.
bool StartShellWindow(const std::string& url, int client_w, int client_h,
                      std::function<void()> on_closed);

}  // namespace zc
