#include "room_plan.h"

#include <map>
#include <set>

namespace zc {

std::vector<RoomAction> PlanRooms(const RoomLayout& want,
                                  const BreakoutState& have) {
  std::vector<RoomAction> plan;

  // Index the live world by name once: room existence, and person -> room.
  std::set<std::string> live_rooms;
  std::map<std::string, std::string> person_room;
  for (const BreakoutRoomInfo& r : have.rooms) {
    live_rooms.insert(r.name);
    for (const std::string& u : r.user_names) person_room[u] = r.name;
  }

  bool all_satisfied = true;

  for (const auto& [room, members] : want.rooms) {
    const bool room_exists = live_rooms.count(room) != 0;
    if (!room_exists) {
      all_satisfied = false;
      if (!have.started) {
        // The SDK edits the room list only in BO_STATUS_EDIT; after start
        // there is nothing to emit -- the executor's ops line names it.
        plan.push_back({RoomActionKind::kCreateRoom, room, ""});
      }
    }
    for (const std::string& person : members) {
      const auto it = person_room.find(person);
      const bool placed = it != person_room.end() && it->second == room;
      if (placed) continue;
      all_satisfied = false;
      if (!have.started) {
        // Pre-start assignment works whether or not the room exists yet in
        // OUR snapshot -- creation above is part of the same convergence
        // pass, and a not-yet-confirmed room simply re-plans next pass.
        plan.push_back({RoomActionKind::kAssignUser, person, room});
      } else if (!room_exists) {
        // Started and the room does not exist: unfixable this pass (see
        // create rule above); nothing to emit for this person either.
      } else if (it == person_room.end()) {
        plan.push_back({RoomActionKind::kAssignRunning, person, room});
      } else {
        plan.push_back({RoomActionKind::kSwitchRunning, person, room});
      }
    }
  }

  // Everything staged but the session is not running: the one action left
  // is the operator's start.
  if (!have.started && !want.rooms.empty() && all_satisfied) {
    plan.push_back({RoomActionKind::kNeedStart, "", ""});
  }
  return plan;
}

}  // namespace zc
