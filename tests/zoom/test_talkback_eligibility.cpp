#include "talkback_eligibility.h"
#include "test_util.h"

void TestTalkbackEligibility() {
  using zc::RosterMember;
  std::vector<RosterMember> roster = {
      {1, "Pat", false, true}, {2, "Web guest", false, false},
      {3, "Unknown", false, false}, {4, "ZComms", false, true},
      {5, "Pat", false, false}};

  ZC_TEST("talent: only confirmed-capable recipients can be assigned");
  ZC_CHECK(zc::CanAssignTalkback(roster, "ZComms", 1, 0, 16));
  for (unsigned int uid : {2u, 3u, 4u, 5u, 99u}) {
    ZC_CHECK(!zc::CanAssignTalkback(roster, "ZComms", uid, 0, 16));
  }
  ZC_CHECK(!zc::CanAssignTalkback(roster, "ZComms", 1, -1, 16));
  ZC_CHECK(!zc::CanAssignTalkback(roster, "ZComms", 1, 16, 16));

  ZC_TEST("talent: stale unsupported membership is hidden by id, not name");
  auto visible = zc::EligibleChannelMembers({1, 2, 3, 4, 5}, roster, "ZComms");
  ZC_CHECK(visible == std::set<unsigned int>{1});
  ZC_CHECK(zc::EligibleChannelMembers({2}, roster, "ZComms").empty());

  ZC_TEST("talent: missing breakout participants retain membership");
  visible = zc::EligibleChannelMembers({1, 2, 99}, roster, "ZComms");
  const std::set<unsigned int> expected{1, 99};
  ZC_CHECK(visible == expected);

  ZC_TEST("talent: capability loss revokes eligibility and regain restores it");
  roster[0].supports_talkback = false;
  std::set<std::pair<int, unsigned int>> intent{{0, 1}, {1, 1}, {2, 99}};
  std::set<unsigned int> assigned{1, 99};
  zc::PruneIneligibleTalkbackIntent(roster, "ZComms", intent, assigned);
  const std::set<std::pair<int, unsigned int>> remote_intent{{2, 99}};
  ZC_CHECK(intent == remote_intent);
  ZC_CHECK(assigned == std::set<unsigned int>{99});
  ZC_CHECK(!zc::CanAssignTalkback(roster, "ZComms", 1, 0, 16));
  ZC_CHECK(zc::EligibleChannelMembers({1}, roster, "ZComms").empty());
  roster[0].supports_talkback = true;
  ZC_CHECK(zc::CanAssignTalkback(roster, "ZComms", 1, 0, 16));
}
