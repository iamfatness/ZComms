#include <vector>

#include "stats.h"
#include "test_util.h"

using namespace zc;

void TestStats() {
  std::printf("TestStats\n");

  {
    ZC_TEST("summarises a known distribution");
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(static_cast<double>(i));
    const Summary s = Summarise(v);
    ZC_CHECK(s.n == 100);
    ZC_CHECK_NEAR(s.min_ms, 1.0, 1e-9);
    ZC_CHECK_NEAR(s.max_ms, 100.0, 1e-9);
    ZC_CHECK_NEAR(s.p50_ms, 50.5, 1e-9);
    ZC_CHECK_NEAR(s.mean_ms, 50.5, 1e-9);
    ZC_CHECK_NEAR(s.p95_ms, 95.05, 0.01);
  }

  {
    ZC_TEST("p95 tracks the tail, not the bulk");
    // The case the kill criterion cares about: a well-behaved median hiding a
    // bad tail must not summarise as healthy.
    std::vector<double> v(95, 100.0);
    v.insert(v.end(), 5, 900.0);
    const Summary s = Summarise(v);
    ZC_CHECK_NEAR(s.p50_ms, 100.0, 1e-9);
    ZC_CHECK(s.p95_ms > 100.0);
    ZC_CHECK(s.jitter_p95_p50_ms > 0.0);
  }

  {
    ZC_TEST("MAD ignores outliers that inflate stddev");
    std::vector<double> v(50, 200.0);
    v.push_back(5000.0);
    const Summary s = Summarise(v);
    ZC_CHECK_NEAR(s.mad_ms, 0.0, 1e-9);
    ZC_CHECK(s.stddev_ms > 100.0);
  }

  {
    ZC_TEST("degenerate inputs do not crash");
    const Summary empty = Summarise({});
    ZC_CHECK(empty.n == 0);
    const Summary one = Summarise({42.0});
    ZC_CHECK(one.n == 1);
    ZC_CHECK_NEAR(one.p50_ms, 42.0, 1e-9);
    ZC_CHECK_NEAR(one.p95_ms, 42.0, 1e-9);
    ZC_CHECK_NEAR(one.stddev_ms, 0.0, 1e-9);
  }
}
