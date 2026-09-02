#include "crash_trap.h"

#include <windows.h>

#include "diag_log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

// src/audio/signal.h (the engine's test-signal generator) shadows the CRT's
// <signal.h> on this include path, which breaks <csignal> outright. Declare
// the one CRT entry point needed instead of renaming a load-bearing header.
// 22 is SIGABRT's documented, ABI-stable value on Windows.
extern "C" {
typedef void(__cdecl* zc_crt_signal_t)(int);
zc_crt_signal_t __cdecl signal(int sig, zc_crt_signal_t handler);
}
namespace {
constexpr int kSigAbrt = 22;  // SIGABRT
}

namespace zc {

// The one exit for every fatal route: the detail goes to the diagnostic
// stream (BindStdio has already pointed that at the log file on an Explorer
// launch), then into a message box the operator can photograph. Termination
// is TerminateProcess, same as main.cpp's HardExit -- returning from here
// would reach sdk.dll's own DLL_PROCESS_DETACH fastfail and bury the story.
[[noreturn]] void Die(const std::string& what) {
  std::printf("\nFATAL: %s\n", what.c_str());
  std::fprintf(stderr, "\nFATAL: %s\n", what.c_str());
  // Twice on purpose. The printf pair goes through the diagnostic tee, which
  // DiagFlush then drains to the file; DiagEmergency writes the same fact
  // directly, taking no CRT lock and no pipe, so a fatal route that fires
  // WHILE another thread holds the stdout lock still lands in the log. The
  // v0.1.10 hang taught the difference between "we tried to log it" and "it
  // is in the file".
  // Flush FIRST, then write the direct copy: that way the ordinary stream's
  // FATAL line lands in the file ahead of the emergency duplicate instead of
  // behind it. Chronology in a crash log is evidence (v0.1.8, the "w"/"a"
  // split that shuffled the first field crash log).
  DiagFlush(2000);
  DiagEmergency("FATAL: " + what);
  const std::string text =
      what +
      "\n\nZComms has to close. This text is also in the newest file under "
      "%APPDATA%\\ZComms\\logs -- photograph either when reporting.";
  // MB_SYSTEMMODAL: the shell window's thread may be the one that died;
  // a system-modal box needs no owner and stays on top of the wreckage.
  MessageBoxA(nullptr, text.c_str(), "ZComms crashed",
              MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);
  TerminateProcess(GetCurrentProcess(), 3);
  for (;;) {}
}

namespace {

void OnTerminate() {
  Die("std::terminate -- " + DescribeActiveCppException());
}

// abort() raises SIGABRT while a handler is installed, BEFORE the __fastfail
// that WER reported as ucrtbase 0xc0000409. This is that crash's named route.
void OnSigAbrt(int) { Die("abort() called (CRT fail-fast path)"); }

// CRT invalid parameter: NOT fatal, by policy. Live-diagnosed 2026-09-01 on
// a work-managed machine: the Zoom SDK's own post-init background thread
// trips this (the handler is process-wide; sdk.dll shares our ucrtbase),
// and the default handler's fail-fast was the whole v0.1.6 "crashes at
// join" -- the log showed auth SUCCESS and the meeting reaching CONNECTING
// underneath the v0.1.7 crash box. Returning from the handler is defined
// behavior: the offending call fails with errno EINVAL and execution
// continues. So: loud, counted, rate-limited -- never dead. Our own code
// hitting this surfaces as a failed CRT call plus these lines.
std::atomic<unsigned> g_invalid_param_count{0};
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                        unsigned, uintptr_t) {
  const unsigned n = g_invalid_param_count.fetch_add(1) + 1;
  if (n <= 5 || n % 100 == 0) {
    std::printf(
        "WARNING: CRT invalid parameter #%u -- some module passed bad "
        "arguments to a CRT call; the call failed with EINVAL and the app "
        "continues (seen from the Zoom SDK's own threads on locked-down "
        "machines)\n",
        n);
    std::fflush(stdout);
  }
}

void OnPureCall() { Die("pure virtual function call"); }

LONG WINAPI OnUnhandledSeh(EXCEPTION_POINTERS* info) {
  unsigned long code = 0;
  const void* addr = nullptr;
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    code = info->ExceptionRecord->ExceptionCode;
    addr = info->ExceptionRecord->ExceptionAddress;
  }
  char base[MAX_PATH] = {};
  const char* base_name = nullptr;
  unsigned long long offset = 0;
  HMODULE mod = nullptr;
  if (addr != nullptr &&
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &mod) &&
      mod != nullptr) {
    char full[MAX_PATH] = {};
    if (GetModuleFileNameA(mod, full, sizeof(full)) != 0) {
      const char* slash = std::strrchr(full, '\\');
      std::snprintf(base, sizeof(base), "%s", slash ? slash + 1 : full);
      base_name = base;
      offset = static_cast<unsigned long long>(
          reinterpret_cast<const char*>(addr) -
          reinterpret_cast<const char*>(mod));
    }
  }
  Die(DescribeSeh(code, base_name, offset));
}

}  // namespace

unsigned InvalidParameterCount() { return g_invalid_param_count.load(); }

const char* SehCodeName(unsigned long code) {
  switch (code) {
    case 0xC0000005UL: return "access violation";
    case 0xC0000409UL: return "fail-fast (stack cookie, abort, or CRT hard error)";
    case 0xC0000374UL: return "heap corruption";
    case 0xC00000FDUL: return "stack overflow";
    case 0xC000001DUL: return "illegal instruction";
    case 0xC0000094UL: return "integer divide by zero";
    case 0xC0000135UL: return "DLL not found";
    case 0xC0000006UL: return "in-page I/O error";
    case 0xE06D7363UL: return "unhandled C++ exception";
    default: return "";
  }
}

std::string DescribeSeh(unsigned long code, const char* module_basename,
                        unsigned long long offset) {
  char buf[512];
  const char* name = SehCodeName(code);
  int n;
  if (name[0] != '\0') {
    n = std::snprintf(buf, sizeof(buf), "unhandled SEH exception 0x%08lX (%s)",
                      code, name);
  } else {
    n = std::snprintf(buf, sizeof(buf), "unhandled SEH exception 0x%08lX",
                      code);
  }
  if (module_basename != nullptr && n > 0 &&
      static_cast<size_t>(n) < sizeof(buf)) {
    std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                  " at %s+0x%llX", module_basename, offset);
  }
  return buf;
}

std::string DescribeActiveCppException() {
  const std::exception_ptr p = std::current_exception();
  if (!p) return "no active C++ exception";
  try {
    std::rethrow_exception(p);
  } catch (const std::exception& e) {
    return std::string(typeid(e).name()) + ": " + e.what();
  } catch (...) {
    return "C++ exception not derived from std::exception";
  }
}

void InstallCrashTrap() {
  std::set_terminate(OnTerminate);
  ::signal(kSigAbrt, OnSigAbrt);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  SetUnhandledExceptionFilter(OnUnhandledSeh);
}

}  // namespace zc
