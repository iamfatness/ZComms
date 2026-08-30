// Sub-production room planning (pure, no SDK includes -- this header is
// what the tests and the panel logic see; breakout.h layers the SDK on top).
//
// The operator expresses a desired layout ONCE (room name -> member display
// names) and PlanRooms diffs it against the live BreakoutState into an
// ordered action list; running a converged state through it yields nothing.
// Everything is keyed by display name: BO user ids are their own string
// GUIDs, unrelated to meeting user ids -- the NAME is the only join
// (delivery-laws work, 2026-08-29).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace zc {

struct BreakoutRoomInfo {
  std::string id;
  std::string name;
  std::vector<std::string> user_names;
};

struct BreakoutState {
  bool enabled = false;
  bool started = false;
  bool in_bo = false;
  std::string my_room;  // empty = the main session
  std::vector<BreakoutRoomInfo> rooms;
};

// Desired layout: room name -> member display names.
struct RoomLayout {
  std::vector<std::pair<std::string, std::vector<std::string>>> rooms;
};

enum class RoomActionKind {
  kCreateRoom,    // arg1 = room name             (pre-start only: the SDK
                  // edits rooms only in BO_STATUS_EDIT)
  kAssignUser,    // arg1 = person, arg2 = room   (pre-start)
  kAssignRunning, // arg1 = person, arg2 = room   (started, user unassigned)
  kSwitchRunning, // arg1 = person, arg2 = room   (started, user elsewhere)
  kNeedStart,     // layout wants rooms but the session is not started
};

struct RoomAction {
  RoomActionKind kind;
  std::string arg1;
  std::string arg2;
};

// Diff desired vs live. Idempotent: a converged state plans {}. Re-entrant
// by design -- every SDK mutation is async-confirmed, so the caller runs
// this again next pass and only the still-missing actions re-emerge.
std::vector<RoomAction> PlanRooms(const RoomLayout& want,
                                  const BreakoutState& have);

}  // namespace zc
