#include "reach.h"
#include "test_util.h"

void TestReach() {
  std::printf("TestReach\n");

  zc::BreakoutState s;
  s.enabled = true;
  s.started = true;
  s.in_bo = true;
  s.my_room = "GREEN ROOM";
  s.rooms = {{"id1", "GREEN ROOM", {"Me", "Pat"}},
             {"id2", "SEG 2 CREW", {"Sam"}}};

  ZC_TEST("reach: same-room member reachable, other-room member named dark");
  auto r = zc::ReachFor(s, {"Pat", "Sam", "Lee"});
  ZC_CHECK(r.reachable.size() == 1);
  ZC_CHECK(!r.reachable.empty() && r.reachable[0] == "Pat");
  ZC_CHECK(r.elsewhere.size() == 2);
  if (r.elsewhere.size() == 2) {
    ZC_CHECK(r.elsewhere[0].first == "Sam");
    ZC_CHECK(r.elsewhere[0].second == "SEG 2 CREW");
    ZC_CHECK(r.elsewhere[1].first == "Lee");  // main floor, station in a BO
    ZC_CHECK(r.elsewhere[1].second == "");    // "" renders as "main"
  }

  ZC_TEST("reach: station on main floor reaches main-floor members only");
  zc::BreakoutState m = s;
  m.in_bo = false;
  m.my_room = "";
  auto r2 = zc::ReachFor(m, {"Pat", "Lee"});
  ZC_CHECK(r2.reachable == std::vector<std::string>{"Lee"});
  ZC_CHECK(r2.elsewhere.size() == 1 &&
           r2.elsewhere[0].second == "GREEN ROOM");

  ZC_TEST("reach: no breakout session = everyone reachable");
  zc::BreakoutState off;
  auto r3 = zc::ReachFor(off, {"Pat", "Sam"});
  ZC_CHECK(r3.reachable.size() == 2 && r3.elsewhere.empty());
}
