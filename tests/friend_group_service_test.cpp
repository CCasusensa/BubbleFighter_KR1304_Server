#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>

#include "features/social/social_registry.h"

namespace {
namespace bf = bubble_fighter::server;
namespace p = bf::protocol;
namespace s = bf::features::social;
using Character = bf::features::characters::CharacterRecord;
struct State {
  std::map<std::uint64_t, s::SocialSnapshot> snapshots;
  std::vector<std::pair<std::uint64_t, p::CompanionFriendGroupRequest>>
      requests;
  p::CompanionFriendGroupStatus status = p::CompanionFriendGroupStatus::kOk;
  bool failure = false;
  bool wrong_owner = false;
};
struct Store final : s::SocialStore {
  explicit Store(std::shared_ptr<State> state) : state(std::move(state)) {}
  s::SocialSnapshot Load(const Character& owner) override {
    if (state->failure)
      throw std::runtime_error("Injected group store failure");
    auto result = state->snapshots.at(owner.character_id);
    if (state->wrong_owner) ++result.owner.account_id;
    return result;
  }
  std::optional<p::ChatBlockEntry> Find(std::uint32_t,
                                        std::u16string_view) override {
    return {};
  }
  s::SocialChange Change(const Character&,
                         const p::CompanionSocialRequest&) override {
    throw std::runtime_error("Unexpected relationship mutation");
  }
  s::SocialGroupChange ChangeGroup(
      const Character& owner,
      const p::CompanionFriendGroupRequest& request) override {
    state->requests.emplace_back(owner.character_id, request);
    return {state->status, Load(owner)};
  }
  p::CompanionSocialStatus CanMessage(const Character&,
                                      std::uint32_t) override {
    return p::CompanionSocialStatus::kNotFriend;
  }
  std::shared_ptr<State> state;
};
}  // namespace

int RunFriendGroupServiceTests() {
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Friend group service line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool failed = false;
    try {
      action();
    } catch (const std::exception&) {
      failed = true;
    }
    check(failed);
  };
  auto state = std::make_shared<State>();
  Character alice{17, 7, u"Alice", 27, 2}, bob{23, 8, u"Bob", 27, 2};
  state->snapshots[17] = {
      alice, {{23, u"Bob", 1, false}}, {}, {{41, u"PrivateA"}}, {{23, 41}}};
  state->snapshots[23] = {
      bob, {{17, u"Alice", 1, false}}, {}, {{42, u"PrivateB"}}, {}};
  auto registry = std::make_shared<s::SocialRegistry>(
      [state] { return std::make_unique<Store>(state); });
  using Batch = std::vector<p::TcpPacket>;
  std::vector<Batch> a, b;
  auto first = registry->Enter(
      alice, [&](Batch batch) { a.push_back(std::move(batch)); });
  auto second =
      registry->Enter(bob, [&](Batch batch) { b.push_back(std::move(batch)); });
  check(a.size() == 1 && a[0].size() == 1 && a[0][0].message_id == 170);
  check(b.size() == 1 && b[0].size() == 1 && b[0][0].message_id == 170);
  rejects([&] { registry->Enter(alice, [](Batch) {}); });
  a.clear();
  b.clear();
  first.Process({});
  check(a.size() == 1 && a.back().size() == 1 &&
        a.back()[0].message_id == 65011);
  check(b.empty());
  first.ProcessGroups({});
  check(a.back().size() == 2 && a.back()[0].message_id == 65011 &&
        a.back()[1].message_id == 65013);
  auto own = p::CompanionFriendGroupsNotification::Decode(a.back()[1]);
  check(own.groups == state->snapshots[17].groups &&
        own.assignments == state->snapshots[17].assignments);
  check(state->requests.back().first == alice.character_id);
  check(b.empty());
  // A failed structural request does not subscribe a legacy peer.
  rejects([&] {
    second.ProcessGroups({p::CompanionFriendGroupAction::kList, 99, 0, 0, {}});
  });
  second.Process({});
  check(b.back().size() == 1 && b.back()[0].message_id == 65011);
  second.ProcessGroups({});
  auto other = p::CompanionFriendGroupsNotification::Decode(b.back().back());
  check(other.groups == state->snapshots[23].groups &&
        other.assignments.empty());
  a.clear();
  b.clear();
  state->snapshots[17].groups[0].name = u"Changed";
  state->status = p::CompanionFriendGroupStatus::kStale;
  first.ProcessGroups({p::CompanionFriendGroupAction::kMove, 42, 23, 0, {}});
  own = p::CompanionFriendGroupsNotification::Decode(a.back().back());
  check(own.status == p::CompanionFriendGroupStatus::kStale &&
        own.action == p::CompanionFriendGroupAction::kMove);
  check(own.groups[0].name == u"Changed" && own.assignments[0].group_id == 41);
  check(b.empty());
  check(state->requests.back().second.expected_group_id == 0 &&
        state->requests.back().second.friend_id == 23);
  // Complete canonical snapshots precede their group view in one sink batch.
  check(
      p::CompanionSocialRosterNotification::Decode(a.back()[0]).rows[0].name ==
      u"Bob");
  state->status = p::CompanionFriendGroupStatus::kOk;
  state->failure = true;
  first.ProcessGroups({});
  check(a.back().size() == 1);
  own = p::CompanionFriendGroupsNotification::Decode(a.back()[0]);
  check(own.status == p::CompanionFriendGroupStatus::kUnavailable &&
        own.groups.empty());
  state->failure = false;
  state->wrong_owner = true;
  first.ProcessGroups({});
  check(p::CompanionFriendGroupsNotification::Decode(a.back()[0]).status ==
        p::CompanionFriendGroupStatus::kUnavailable);
  state->wrong_owner = false;
  state->snapshots[17].assignments[0].friend_id = 99;
  first.ProcessGroups({});
  check(p::CompanionFriendGroupsNotification::Decode(a.back()[0]).status ==
        p::CompanionFriendGroupStatus::kUnavailable);
  state->snapshots[17].assignments[0].friend_id = 23;
  state->snapshots[17].groups.push_back(state->snapshots[17].groups[0]);
  first.ProcessGroups({});
  check(p::CompanionFriendGroupsNotification::Decode(a.back()[0]).status ==
        p::CompanionFriendGroupStatus::kUnavailable);
  state->snapshots[17].groups.pop_back();
  first.RefreshName();
  check(a.back().size() == 2 && b.back().size() == 2);
  check(p::CompanionFriendGroupsNotification::Decode(b.back()[1])
            .groups[0]
            .name == u"PrivateB");
  auto moved = std::move(first);
  rejects([&] { first.ProcessGroups({}); });
  moved.ProcessGroups({});
  check(p::CompanionFriendGroupsNotification::Decode(a.back().back())
            .groups[0]
            .name == u"Changed");
  moved.Release();
  check(p::CompanionSocialRosterNotification::Decode(b.back()[0])
            .rows[0]
            .online == false);
  rejects([&] { moved.ProcessGroups({}); });
  a.clear();
  auto reconnect = registry->Enter(
      alice, [&](Batch batch) { a.push_back(std::move(batch)); });
  check(a.size() == 1 && a[0].size() == 1 && a[0][0].message_id == 170);
  // Fresh leases do not inherit the old connection's extension subscription.
  reconnect.Process({});
  check(a.back().size() == 1 && a.back()[0].message_id == 65011);
  reconnect.ProcessGroups({});
  check(p::CompanionFriendGroupsNotification::Decode(a.back().back())
            .groups[0]
            .name == u"Changed");
  reconnect.Release();
  second.Release();
  a.clear();
  auto group_only = registry->Enter(
      alice, [&](Batch batch) { a.push_back(std::move(batch)); });
  group_only.ProcessGroups({});
  check(a.back().size() == 1 && a.back()[0].message_id == 65013);
  std::cout << "Friend group service: " << checks << " checks passed.\n";
  return checks;
}
