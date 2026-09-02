// The WebView2 host. See shell_window.h for the contract.
//
// Threading: everything here lives on one dedicated STA thread -- the window,
// the message pump, and every WebView2 COM object. WebView2 controllers are
// bound to the thread that created them, and the main thread's job is pumping
// the Zoom SDK; neither may ever wait on the other. The only cross-thread
// traffic is the on_closed callback, which flips an atomic in main.

#include "shell_window.h"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <wrl.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "WebView2.h"
#include "diag_log.h"
#include "stall_watch.h"

namespace zc {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Owned by the shell thread only.
ComPtr<ICoreWebView2Controller> g_controller;

// --- The UI-thread watchdog -------------------------------------------------
// v0.1.10 wedged on the owner's machine and Windows logged AppHangB1 --
// "zcomms.exe stopped interacting with Windows and was closed". That class is
// specifically about a top-level window whose thread stops servicing messages,
// which here is THIS thread, not the main loop. The v0.1.7 crash trap could
// not fire (nothing faulted) and the main-loop watchdog was watching the wrong
// loop, so the incident left a WER entry and nothing else.
//
// So the pump beats: once per iteration, and once per WM_TIMER so an IDLE
// GetMessageW (which blocks, legitimately, for as long as nothing happens)
// still counts as alive. A separate thread notices when the beat stops and
// writes the fact -- with this thread's id, the duration, and what the main
// loop was doing -- through DiagEmergency, which takes neither the CRT stdout
// lock nor the diagnostic pipe. Reporting a hang down a channel the hang can
// block is how the last one stayed invisible.
constexpr UINT_PTR kBeatTimerId = 1;
constexpr UINT kBeatTimerMs = 500;
// Well above any legitimate pause. WebView2 creation and navigation are async;
// nothing on this thread is allowed to take four seconds.
constexpr int64_t kStallThresholdMs = 4000;

std::atomic<long long> g_pump_beat_ms{0};
std::atomic<unsigned long> g_shell_tid{0};
std::atomic<bool> g_pump_running{false};
std::atomic<HWND> g_hwnd{nullptr};

// The self-test's deliberate stall (see ShellStallSelfTest).
constexpr UINT kMsgStallSelfTest = WM_APP + 1;

void BeatPump() {
  g_pump_beat_ms.store(static_cast<long long>(GetTickCount64()));
}

void ShellWatchdogThread() {
  StallWatch watch(kStallThresholdMs);
  while (g_pump_running.load()) {
    Sleep(500);
    const int64_t now_ms = static_cast<int64_t>(GetTickCount64());
    watch.Beat(g_pump_beat_ms.load());
    int64_t age = 0;
    if (watch.Poll(now_ms, &age)) {
      DiagEmergency(
          "[watchdog] UI MESSAGE PUMP STALLED " + std::to_string(age) +
          " ms -- shell thread " + std::to_string(g_shell_tid.load()) +
          " is not servicing messages; Windows reports this as AppHangB1 and "
          "will kill the process. Main loop was in: " + MainPhaseDescription());
    }
    int64_t lasted = 0;
    if (watch.ConsumeRecovery(&lasted)) {
      DiagEmergency("[watchdog] UI message pump recovered after " +
                    std::to_string(lasted) + " ms");
    }
  }
}

// The panel's designed client size; the frame is added around it so the page
// renders at exactly the size it was designed (and screenshot-approved) at.
int g_client_w = 1000;
int g_client_h = 640;

LRESULT CALLBACK ShellProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      if (g_controller) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_controller->put_Bounds(rc);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      // The panel degrades below its designed size; don't let it. Sizes are
      // DIPs; scale by the window's current DPI.
      const UINT dpi = GetDpiForWindow(hwnd);
      RECT frame{0, 0, MulDiv(g_client_w, (int)dpi, 96),
                 MulDiv(g_client_h, (int)dpi, 96)};
      AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize.x = (frame.right - frame.left) * 3 / 4;
      mmi->ptMinTrackSize.y = (frame.bottom - frame.top) * 3 / 4;
      return 0;
    }
    case WM_DPICHANGED: {
      // Dragged to a monitor with a different scale: take the suggested
      // rect; WM_SIZE re-bounds the WebView, which re-rasterizes crisply.
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SETFOCUS:
      // Keyboard straight into the panel: TALK keys, digits, SPACE all-call.
      if (g_controller) {
        g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
      return 0;
    case WM_TIMER:
      // The idle beat. GetMessageW blocks indefinitely when nothing is
      // happening, which is healthy but indistinguishable from wedged without
      // this: a pump that can still deliver a timer is servicing messages.
      if (wp == kBeatTimerId) {
        BeatPump();
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    case kMsgStallSelfTest:
      // Deliberately does NOT beat: this is what a swallowed message looks
      // like, and the watchdog must say so while it is happening.
      std::printf("[ui] selftest: blocking the message pump for %d s\n",
                  static_cast<int>(wp));
      Sleep(static_cast<DWORD>(wp) * 1000);
      std::printf("[ui] selftest: pump released\n");
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

// WebView2 profile data (cookies, cache) must live somewhere writable; the
// exe may sit in a read-only install location.
std::wstring UserDataDir() {
  wchar_t base[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
    GetTempPathW(MAX_PATH, base);
  }
  std::wstring dir = std::wstring(base) + L"\\ZComms";
  CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\WebView2";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

void ShellThread(std::wstring url, std::function<void()> on_closed) {
  // DPI awareness is declared at process start (main.cpp) -- it cannot be
  // changed once any window exists. Everything here computes in DIPs and
  // scales by the live DPI.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc{};
  wc.lpfnWndProc = ShellProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ZCommsShell";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0x1b, 0x1d, 0x21));  // panel iron
  // Icon resource 1 in version.rc (tools/make-icon.py renders it).
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);

  // The designed 1000x640 is logical (DIP); the window is created in
  // physical pixels for wherever it lands.
  const UINT dpi = GetDpiForSystem();
  RECT frame{0, 0, MulDiv(g_client_w, (int)dpi, 96),
             MulDiv(g_client_h, (int)dpi, 96)};
  AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  const int w = frame.right - frame.left;
  const int h = frame.bottom - frame.top;
  const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
  const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
  HWND hwnd = CreateWindowExW(0, L"ZCommsShell", L"ZComms", WS_OVERLAPPEDWINDOW,
                              x, y, w, h, nullptr, nullptr, wc.hInstance,
                              nullptr);
  if (!hwnd) {
    if (on_closed) on_closed();
    CoUninitialize();
    return;
  }
  // Dark native title bar (the default one is white against a dark panel).
  // Attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on Win10 20H1+; on older
  // builds the call fails harmlessly and the bar stays light. Set before
  // ShowWindow so the first paint is already dark.
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                        sizeof(dark));
  ShowWindow(hwnd, SW_SHOW);

  g_hwnd.store(hwnd);
  g_shell_tid.store(GetCurrentThreadId());
  BeatPump();
  g_pump_running.store(true);
  SetTimer(hwnd, kBeatTimerId, kBeatTimerMs, nullptr);
  std::thread(ShellWatchdogThread).detach();
  std::printf("[ui] shell window up on thread %lu (message pump watched)\n",
              GetCurrentThreadId());

  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, UserDataDir().c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [hwnd, url](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
              DestroyWindow(hwnd);
              return hr;
            }
            env->CreateCoreWebView2Controller(
                hwnd,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [hwnd, url](HRESULT hr2,
                                ICoreWebView2Controller* ctrl) -> HRESULT {
                      if (FAILED(hr2) || !ctrl) {
                        DestroyWindow(hwnd);
                        return hr2;
                      }
                      g_controller = ctrl;
                      RECT rc;
                      GetClientRect(hwnd, &rc);
                      ctrl->put_Bounds(rc);
                      ComPtr<ICoreWebView2> web;
                      ctrl->get_CoreWebView2(&web);
                      if (web) {
                        ComPtr<ICoreWebView2Settings> st;
                        web->get_Settings(&st);
                        if (st) {
                          // It's a hardware panel, not a browser: no context
                          // menu, no pinch-zoom. Dev tools stay available --
                          // they cost nothing and earn their keep the first
                          // time the panel misbehaves in the field.
                          st->put_AreDefaultContextMenusEnabled(FALSE);
                          st->put_IsZoomControlEnabled(FALSE);
                          st->put_IsStatusBarEnabled(FALSE);
                        }
                        web->Navigate(url.c_str());
                      }
                      ctrl->MoveFocus(
                          COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    // Beat BEFORE dispatch as well as after: a handler that never returns is
    // exactly the case being watched, and the pre-beat is what makes the
    // reported stall duration start at the message that swallowed the thread.
    BeatPump();
    TranslateMessage(&m);
    DispatchMessageW(&m);
    BeatPump();
  }
  g_pump_running.store(false);
  g_hwnd.store(nullptr);
  g_controller.Reset();
  if (on_closed) on_closed();
  CoUninitialize();
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

}  // namespace

void ShellStallSelfTest(int seconds) {
  const HWND h = g_hwnd.load();
  if (h == nullptr) {
    std::printf("[ui] selftest: no shell window to stall\n");
    return;
  }
  PostMessageW(h, kMsgStallSelfTest, static_cast<WPARAM>(seconds), 0);
}

bool ShellWindowAvailable() {
  wchar_t* ver = nullptr;
  const HRESULT hr =
      GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
  const bool ok = SUCCEEDED(hr) && ver != nullptr;
  if (ver) CoTaskMemFree(ver);
  return ok;
}

bool StartShellWindow(const std::string& url, int client_w, int client_h,
                      std::function<void()> on_closed) {
  if (!ShellWindowAvailable()) return false;
  g_client_w = client_w;
  g_client_h = client_h;
  // Detached on purpose: the window outlives interest in joining it -- app
  // exit is HardExit (TerminateProcess, forced by sdk.dll's detach crash),
  // which tears the thread down without ever unwinding it.
  std::thread(ShellThread, Widen(url), std::move(on_closed)).detach();
  return true;
}

}  // namespace zc
