// The WebView2 host. See shell_window.h for the contract.
//
// Threading: everything here lives on one dedicated STA thread -- the window,
// the message pump, and every WebView2 COM object. WebView2 controllers are
// bound to the thread that created them, and the main thread's job is pumping
// the Zoom SDK; neither may ever wait on the other. The only cross-thread
// traffic is the on_closed callback, which flips an atomic in main.

#include "shell_window.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <wrl.h>

#include <cstdio>
#include <thread>

#include "WebView2.h"

namespace zc {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Owned by the shell thread only.
ComPtr<ICoreWebView2Controller> g_controller;

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
      // The panel degrades below its designed size; don't let it.
      RECT frame{0, 0, g_client_w, g_client_h};
      AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize.x = (frame.right - frame.left) * 3 / 4;
      mmi->ptMinTrackSize.y = (frame.bottom - frame.top) * 3 / 4;
      return 0;
    }
    case WM_SETFOCUS:
      // Keyboard straight into the panel: TALK keys, digits, SPACE all-call.
      if (g_controller) {
        g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
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
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc{};
  wc.lpfnWndProc = ShellProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ZCommsShell";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0x1b, 0x1d, 0x21));  // panel iron
  RegisterClassW(&wc);

  RECT frame{0, 0, g_client_w, g_client_h};
  AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
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
  ShowWindow(hwnd, SW_SHOW);

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
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
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
