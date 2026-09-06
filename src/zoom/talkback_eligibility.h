// SDK-independent eligibility shared by assignment and panel projection.
#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace zc {

struct RosterMember {
  unsigned int user_id = 0;
  std::string name;
  bool is_host = false;
  bool supports_talkback = false;
};

inline bool IsTalkbackEligible(const RosterMember& member,
                               const std::string& station_name) {
  // The SDK can expose the station under a fresh id during a room move.
  return member.supports_talkback && member.name != station_name;
}

inline bool CanAssignTalkback(const std::vector<RosterMember>& roster,
                              const std::string& station_name,
                              unsigned int user_id, int slot, int slots) {
  if (slot < 0 || slot >= slots) return false;
  for (const auto& member : roster) {
    if (member.user_id == user_id)
      return IsTalkbackEligible(member, station_name);
  }
  return false;  // Unknown capability is not permission to assign.
}

inline void PruneIneligibleTalkbackIntent(
    const std::vector<RosterMember>& roster, const std::string& station_name,
    std::set<std::pair<int, unsigned int>>& intent,
    std::set<unsigned int>& auto_assigned) {
  for (const auto& member : roster) {
    if (IsTalkbackEligible(member, station_name)) continue;
    auto_assigned.erase(member.user_id);
    for (auto it = intent.begin(); it != intent.end();) {
      if (it->second == member.user_id) it = intent.erase(it);
      else ++it;
    }
  }
}

inline std::set<unsigned int> EligibleChannelMembers(
    std::set<unsigned int> members, const std::vector<RosterMember>& roster,
    const std::string& station_name) {
  for (const auto& member : roster) {
    if (!IsTalkbackEligible(member, station_name)) members.erase(member.user_id);
  }
  // Absence from a room-local roster does not mean ineligible: preserve
  // remote-room membership until the SDK supplies positive evidence.
  return members;
}

}  // namespace zc
