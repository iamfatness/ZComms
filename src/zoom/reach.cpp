#include "reach.h"

namespace zc {

std::string RoomOfName(const BreakoutState& s, const std::string& name) {
  for (const BreakoutRoomInfo& r : s.rooms) {
    for (const std::string& u : r.user_names) {
      if (u == name) return r.name;
    }
  }
  return "";  // main floor (or not visible to us)
}

ChannelReach ReachFor(const BreakoutState& s,
                      const std::vector<std::string>& member_names) {
  ChannelReach out;
  if (!s.started) {
    // No breakout session: one room, everyone in it.
    out.reachable = member_names;
    return out;
  }
  for (const std::string& name : member_names) {
    const std::string room = RoomOfName(s, name);
    if (room == s.my_room) {
      out.reachable.push_back(name);
    } else {
      out.elsewhere.emplace_back(name, room);
    }
  }
  return out;
}

}  // namespace zc
