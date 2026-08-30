#include "duck_plan.h"
#include "test_util.h"

void TestDuckPlan() {
  std::printf("TestDuckPlan\n");

  ZC_TEST("duck: a newly ready channel gets unity immediately");
  // Zoom ducks a channel member's meeting audio BY DEFAULT -- merely being
  // placed in a channel reduces their meeting volume (CoreVideo, live,
  // talent-reported). Unity at creation is the antidote; channel-scoped, so
  // one call covers late joiners.
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b1u, 0u, 1000, &a));
    ZC_CHECK(a.slot == 0);
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kUnity, 1e-6);
    p.Confirm(a);
    ZC_CHECK(!p.Next(0b1u, 0u, 2000, &a));  // converged: no further calls
  }

  ZC_TEST("duck: one call per 300 ms pace window");
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b11u, 0u, 1000, &a));
    p.Confirm(a);
    ZC_CHECK(!p.Next(0b11u, 0u, 1200, &a));  // 200 ms later: gated
    ZC_CHECK(p.Next(0b11u, 0u, 1300, &a));   // 300 ms later: released
    ZC_CHECK(a.slot == 1);
    p.Confirm(a);
  }

  ZC_TEST("duck: keyed channel ducks, released channel restores unity");
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b1u, 0u, 1000, &a));
    p.Confirm(a);  // unity applied
    ZC_CHECK(p.Next(0b1u, 0b1u, 2000, &a));  // key down -> duck
    ZC_CHECK(a.slot == 0);
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kDuck, 1e-6);
    p.Confirm(a);
    ZC_CHECK(!p.Next(0b1u, 0b1u, 3000, &a));  // held: steady
    ZC_CHECK(p.Next(0b1u, 0u, 4000, &a));     // key up -> restore
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kUnity, 1e-6);
  }

  ZC_TEST("duck: a channel that arrives already keyed ducks directly");
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b1u, 0b1u, 1000, &a));
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kDuck, 1e-6);
  }

  ZC_TEST("duck: a refused call is retried after backoff, not before");
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b1u, 0u, 1000, &a));
    p.Fail(1000);  // e.g. code 18 -- the per-call rate limit
    ZC_CHECK(!p.Next(0b1u, 0u, 1000 + zc::DuckPlanner::kRetryMs - 1, &a));
    ZC_CHECK(p.Next(0b1u, 0u, 1000 + zc::DuckPlanner::kRetryMs, &a));
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kUnity, 1e-6);
  }

  ZC_TEST("duck: a slot that drops out of ready is forgotten, then re-set");
  {
    zc::DuckPlanner p;
    zc::VolumeAction a;
    ZC_CHECK(p.Next(0b1u, 0u, 1000, &a));
    p.Confirm(a);
    ZC_CHECK(!p.Next(0b0u, 0u, 2000, &a));  // gone: nothing to do
    ZC_CHECK(p.Next(0b1u, 0u, 3000, &a));   // back (re-created): unity again
    ZC_CHECK_NEAR(a.volume, zc::DuckPlanner::kUnity, 1e-6);
  }
}
