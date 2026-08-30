// Per-channel reachability truth (pure, no SDK includes).
//
// Delivery law #2: talkback never crosses breakout rooms. A channel is only
// as real as the members who can hear it FROM WHERE THE STATION IS -- so
// the panel renders each channel's membership split into reachable-now and
// present-but-elsewhere, and a key with zero reachable members is refused
// the same way keyed-but-empty is.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "room_plan.h"

namespace zc {

struct ChannelReach {
  std::vector<std::string> reachable;                          // same room
  std::vector<std::pair<std::string, std::string>> elsewhere;  // (name, room)
};

// One room-resolution rule for the whole product: a name found in a room's
// user list is in that room; a name in no list is on the main floor.
// BreakoutRooms::RoomOf delegates here.
std::string RoomOfName(const BreakoutState& s, const std::string& name);

// Station room comes from s.my_room ("" = main floor). elsewhere carries the
// room name, "" meaning the main floor (renders as "main").
ChannelReach ReachFor(const BreakoutState& s,
                      const std::vector<std::string>& member_names);

}  // namespace zc
