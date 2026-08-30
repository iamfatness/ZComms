#include <cstdio>

#include "test_util.h"

int main() {
  std::printf("ZComms audio engine tests\n\n");
  TestEnvelope();
  TestLimiter();
  TestFrameAccumulator();
  TestSampleRing();
  TestFrameRing();
  TestAec();
  TestRoomPlan();
  TestReach();
  TestSignalProtocol();
  TestSignalOutbox();

  const auto& failures = zctest::Failures();
  std::printf("\n");
  if (failures.empty()) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("%zu FAILURE(S):\n", failures.size());
  for (const auto& f : failures) {
    std::printf("  [%s] %s\n", f.test.c_str(), f.detail.c_str());
  }
  return 1;
}
