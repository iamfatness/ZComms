// Last-resort crash visibility. Motivated by a real field crash
// (2026-08-31): v0.1.6 died at join on a work-managed machine as WER event
// "ucrtbase.dll 0xc0000409" -- the CRT fail-fast, which is what an uncaught
// C++ exception, abort(), or a CRT invalid parameter all become in a
// Windows-subsystem exe: no console, no message, a log that just stops. That
// machine's policy allowed photographing the screen but not copying files
// off, so the diagnosis surface must BE the screen: every fatal route logs
// what died and shows the same text in a message box before going down.
#pragma once

#include <string>

namespace zc {

// Installs the process-wide handlers (std::set_terminate, SIGABRT, CRT
// invalid-parameter, purecall, SetUnhandledExceptionFilter). Call once,
// immediately after BindStdio() so the FATAL line lands in the same log.
void InstallCrashTrap();

// The one fatal exit: logs "FATAL: <what>", flushes, shows the same text in
// a system-modal message box, then TerminateProcess (never a CRT exit --
// sdk.dll fastfails in its own DLL_PROCESS_DETACH and would bury the story).
[[noreturn]] void Die(const std::string& what);

// Pure, unit-tested pieces of the message the handlers assemble.
// Human name for the SEH codes worth recognizing on sight; "" for the rest.
const char* SehCodeName(unsigned long code);
// "unhandled SEH exception 0x... (name) at module+0xoffset"; the location
// clause is omitted when the faulting address resolved to no module.
std::string DescribeSeh(unsigned long code, const char* module_basename,
                        unsigned long long offset);
// Type + what() of the exception currently in flight (valid inside a catch
// block or a terminate handler); a fixed phrase when there is none.
std::string DescribeActiveCppException();

}  // namespace zc
