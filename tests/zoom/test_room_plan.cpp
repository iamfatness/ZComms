#include "room_plan.h"
#include "test_util.h"

void TestRoomPlan() {
  std::printf("TestRoomPlan\n");

  ZC_TEST("room_plan: empty state, two-room layout -> creates + assigns");
  zc::RoomLayout want;
  want.rooms = {{"GREEN ROOM", {"Pat"}}, {"SEG 2 CREW", {"Sam", "Alex"}}};
  zc::BreakoutState have;
  have.enabled = true;
  auto plan = zc::PlanRooms(want, have);
  int creates = 0, assigns = 0;
  for (const auto& a : plan) {
    if (a.kind == zc::RoomActionKind::kCreateRoom) ++creates;
    if (a.kind == zc::RoomActionKind::kAssignUser) ++assigns;
  }
  ZC_CHECK(creates == 2);
  ZC_CHECK(assigns == 3);

  ZC_TEST("room_plan: converged state plans nothing");
  zc::BreakoutState done;
  done.enabled = true;
  done.started = true;
  done.rooms = {{"id1", "GREEN ROOM", {"Pat"}},
                {"id2", "SEG 2 CREW", {"Sam", "Alex"}}};
  ZC_CHECK(zc::PlanRooms(want, done).empty());

  ZC_TEST("room_plan: started session moves people via running-BO verbs");
  zc::BreakoutState live;
  live.enabled = true;
  live.started = true;
  live.rooms = {{"id1", "GREEN ROOM", {"Pat", "Sam"}},
                {"id2", "SEG 2 CREW", {"Alex"}}};
  auto plan2 = zc::PlanRooms(want, live);  // Sam belongs in SEG 2 CREW
  ZC_CHECK(plan2.size() == 1);
  if (plan2.size() == 1) {
    ZC_CHECK(plan2[0].kind == zc::RoomActionKind::kSwitchRunning);
    ZC_CHECK(plan2[0].arg1 == "Sam");
    ZC_CHECK(plan2[0].arg2 == "SEG 2 CREW");
  }

  ZC_TEST("room_plan: missing room after start emits no create (edit is pre-start only)");
  zc::RoomLayout want3;
  want3.rooms = {{"NEW ROOM", {"Pat"}}};
  auto plan3 = zc::PlanRooms(want3, live);
  for (const auto& a : plan3) {
    ZC_CHECK(a.kind != zc::RoomActionKind::kCreateRoom);
  }

  ZC_TEST("room_plan: rooms wanted but session not started emits kNeedStart");
  zc::BreakoutState idle;
  idle.enabled = true;
  idle.rooms = {{"id1", "GREEN ROOM", {"Pat"}},
                {"id2", "SEG 2 CREW", {"Sam", "Alex"}}};
  auto plan4 = zc::PlanRooms(want, idle);
  bool need_start = false;
  for (const auto& a : plan4) {
    need_start |= (a.kind == zc::RoomActionKind::kNeedStart);
  }
  ZC_CHECK(need_start);
}
