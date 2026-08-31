// Pins the pure half of the crash trap: the strings an operator will
// photograph off a locked-down machine. The field crash that motivated all
// of this (2026-08-31) surfaced as nothing but "ucrtbase.dll 0xc0000409" in
// Event Viewer; these strings are the difference between that and an answer.
#include <stdexcept>
#include <string>

#include "crash_trap.h"
#include "test_util.h"

void TestCrashTrap() {
  ZC_TEST("crash_trap: SEH code names");
  ZC_CHECK(std::string(zc::SehCodeName(0xC0000005UL)) == "access violation");
  ZC_CHECK(std::string(zc::SehCodeName(0xC0000409UL)) ==
           "fail-fast (stack cookie, abort, or CRT hard error)");
  ZC_CHECK(std::string(zc::SehCodeName(0xC0000374UL)) == "heap corruption");
  ZC_CHECK(std::string(zc::SehCodeName(0xE06D7363UL)) ==
           "unhandled C++ exception");
  ZC_CHECK(std::string(zc::SehCodeName(0x12345678UL)).empty());

  ZC_TEST("crash_trap: SEH description formatter");
  ZC_CHECK(zc::DescribeSeh(0xC0000005UL, "zSDK.dll", 0x1234) ==
           "unhandled SEH exception 0xC0000005 (access violation) at "
           "zSDK.dll+0x1234");
  // Unknown code: no parenthetical, module still named.
  ZC_CHECK(zc::DescribeSeh(0x12345678UL, "zcomms.exe", 0x10) ==
           "unhandled SEH exception 0x12345678 at zcomms.exe+0x10");
  // Address outside any module: no location clause at all.
  ZC_CHECK(zc::DescribeSeh(0xC0000005UL, nullptr, 0) ==
           "unhandled SEH exception 0xC0000005 (access violation)");

  ZC_TEST("crash_trap: active C++ exception description");
  ZC_CHECK(zc::DescribeActiveCppException() == "no active C++ exception");
  try {
    throw std::out_of_range("invalid string position");
  } catch (...) {
    const std::string d = zc::DescribeActiveCppException();
    ZC_CHECK(d.find("out_of_range") != std::string::npos);
    ZC_CHECK(d.find("invalid string position") != std::string::npos);
  }
  try {
    throw 42;  // non-std payloads must still describe, not re-crash
  } catch (...) {
    ZC_CHECK(zc::DescribeActiveCppException() ==
             "C++ exception not derived from std::exception");
  }
}
