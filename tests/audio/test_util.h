// Minimal assertion harness. Same shape as the spike's, deliberately: this
// repo has enough vendored trees without adding a test framework for a
// handful of value checks.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace zctest {

struct Failure {
  std::string test;
  std::string detail;
};

inline std::vector<Failure>& Failures() {
  static std::vector<Failure> f;
  return f;
}

inline const char*& CurrentTest() {
  static const char* t = "";
  return t;
}

inline void Fail(const std::string& detail) {
  Failures().push_back({CurrentTest(), detail});
  std::printf("    FAIL: %s\n", detail.c_str());
}

#define ZC_CHECK(cond)                                             \
  do {                                                             \
    if (!(cond)) {                                                 \
      ::zctest::Fail(std::string(#cond) + "  [" + __FILE__ + ":" + \
                     std::to_string(__LINE__) + "]");              \
    }                                                              \
  } while (0)

#define ZC_CHECK_NEAR(actual, expected, tol)                                   \
  do {                                                                         \
    const double a_ = (actual), e_ = (expected), t_ = (tol);                    \
    if (!(std::fabs(a_ - e_) <= t_)) {                                          \
      char buf_[256];                                                           \
      std::snprintf(buf_, sizeof(buf_),                                         \
                    "%s = %.6f, expected %.6f +/- %.6f  [%s:%d]", #actual, a_,  \
                    e_, t_, __FILE__, __LINE__);                                \
      ::zctest::Fail(buf_);                                                     \
    }                                                                           \
  } while (0)

#define ZC_TEST(name)             \
  ::zctest::CurrentTest() = name; \
  std::printf("  %s\n", name);

}  // namespace zctest

void TestEnvelope();
void TestLimiter();
void TestFrameAccumulator();
void TestSampleRing();
void TestFrameRing();
void TestAec();
void TestRoomPlan();
void TestReach();
void TestSignalProtocol();
void TestSignalOutbox();
void TestDuckPlan();
void TestSignalGate();
void TestChannelMix();
void TestExternFeed();
void TestCrashTrap();
