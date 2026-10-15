#include <algorithm>
#include <iostream>
#include <map>
#include <source_location>

#include "features/social/social_registry.h"

namespace {
namespace bf = bubble_fighter::server;
namespace p = bf::protocol;
namespace s = bf::features::social;
using Character = bf::features::characters::CharacterRecord;
using Batch = std::vector<p::TcpPacket>;
struct State {
  std::map<std::uint64_t, s::SocialSnapshot> snapshots;
  std::map<std::uint64_t, std::vector<p::CompanionQueuedMessage>> messages;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> acknowledged;
  std::uint64_t next_id = 0x100000001;
  std::uint64_t last_clock = 0;
  p::CompanionSocialStatus status = p::CompanionSocialStatus::kOk;
  bool fail_queue = false, fail_read = false, fail_ack = false;
  bool wrong_owner = false, invalid_sender = false;
};
struct Store final : s::SocialStore {
  explicit Store(std::shared_ptr<State> state) : state(std::move(state)) {}
  s::SocialSnapshot Load(const Character& owner) override {
    return state->snapshots.at(owner.character_id);
  }
  std::optional<p::ChatBlockEntry> Find(std::uint32_t id,
                                        std::u16string_view) override {
    if (!state->snapshots.contains(id)) return {};
    return p::ChatBlockEntry{id, state->snapshots.at(id).owner.character_name};
  }
  s::SocialChange Change(const Character&,
                         const p::CompanionSocialRequest&) override {
    throw std::runtime_error("Unexpected mailbox relationship mutation");
  }
  p::CompanionSocialStatus CanMessage(const Character&,
                                      std::uint32_t) override {
    return state->status;
  }
  s::SocialMessageChange QueueMessage(const Character& owner,
                                      const p::CompanionSocialRequest& request,
                                      std::uint64_t now) override {
    if (state->fail_queue) throw std::runtime_error("Injected queue failure");
    if (state->status != p::CompanionSocialStatus::kOk) return {state->status};
    state->last_clock = now;
    const auto id = state->next_id++;
    const auto name =
        state->snapshots.at(owner.character_id).owner.character_name;
    state->messages[request.target_id].push_back(
        {id, static_cast<std::uint32_t>(owner.character_id), now, name,
         request.text});
    return {p::CompanionSocialStatus::kOk, Find(request.target_id, {}), name,
            id};
  }
  s::SocialMailboxSnapshot LoadMailbox(const Character& owner,
                                       std::uint64_t now) override {
    if (state->fail_read)
      throw std::runtime_error("Injected mailbox read failure");
    state->last_clock = now;
    s::SocialMailboxSnapshot result{p::CompanionMailboxStatus::kOk, Load(owner),
                                    state->messages[owner.character_id]};
    if (result.messages.size() > 20) result.messages.resize(20);
    if (state->wrong_owner) ++result.snapshot.owner.account_id;
    if (state->invalid_sender && !result.messages.empty())
      result.messages[0].sender_id = 99;
    return result;
  }
  void AcknowledgeMessage(const Character& owner, std::uint64_t id) override {
    if (state->fail_ack)
      throw std::runtime_error("Injected acknowledgement failure");
    state->acknowledged.emplace_back(owner.character_id, id);
    std::erase_if(
        state->messages[owner.character_id],
        [id](const auto& message) { return message.message_id == id; });
  }
  std::shared_ptr<State> state;
};
}  // namespace

int RunOfflineMessageServiceTests() {
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Mailbox service line " +
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
  state->snapshots[17] = {alice, {{23, u"Bob", 1, false}}, {}};
  state->snapshots[23] = {bob, {{17, u"Alice", 1, false}}, {}};
  const std::uint64_t now = 1792000000;
  const auto create = [&] {
    return std::make_shared<s::SocialRegistry>(
        [state] { return std::make_unique<Store>(state); },
        [=] { return now; });
  };
  auto registry = create();
  std::vector<Batch> a, b;
  const auto enter_a = [&] {
    return registry->Enter(
        alice, [&](Batch packets) { a.push_back(std::move(packets)); });
  };
  const auto enter_b = [&] {
    return registry->Enter(
        bob, [&](Batch packets) { b.push_back(std::move(packets)); });
  };
  auto first = enter_a();
  first.Process({});
  a.clear();
  first.Process({p::CompanionSocialAction::kMessage, 23, {}, u"legacy"});
  check(p::CompanionSocialRosterNotification::Decode(a.back()[0]).status ==
        p::CompanionSocialStatus::kOffline);
  check(state->messages[23].empty());
  rejects([&] {
    first.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, 1});
  });
  first.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(a.back().back())
            .messages.empty());
  first.Process({p::CompanionSocialAction::kMessage, 23, {}, u"stored😀"});
  const auto id = state->messages[23][0].message_id;
  check(id == 0x100000001 && state->last_clock == now);
  check(a.back()[0].message_id == 65015 && a.back()[1].message_id == 509);
  check(p::CompanionPrivateMessageNotification::Decode(a.back()[0]).text ==
        u"stored😀");
  auto second = enter_b();
  second.Process({});
  check(std::none_of(b.back().begin(), b.back().end(), [](const auto& packet) {
    return packet.message_id == 65017;
  }));
  second.ProcessGroups({});
  second.ProcessMailbox({});
  check(b.back().size() == 3 && b.back()[0].message_id == 65011 &&
        b.back()[1].message_id == 65013);
  auto received = p::CompanionMailboxNotification::Decode(b.back().back());
  check(received.messages == state->messages[23]);
  second.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(b.back().back()).messages ==
        received.messages);
  rejects([&] {
    first.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id});
  });
  rejects([&] {
    second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id + 1});
  });
  check(state->acknowledged.empty() && state->messages[23].size() == 1);
  state->fail_ack = true;
  second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id});
  check(p::CompanionMailboxNotification::Decode(b.back().back()).status ==
        p::CompanionMailboxStatus::kUnavailable);
  check(state->messages[23].size() == 1);
  state->fail_ack = false;
  second.Release();
  second = enter_b();
  rejects([&] {
    second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id});
  });
  second.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(b.back().back()).messages ==
        received.messages);
  second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id});
  check(state->messages[23].empty() && state->acknowledged.size() == 1);
  const auto count = b.size();
  second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge, id});
  check(state->acknowledged.size() == 1 && b.size() == count);
  state->fail_queue = true;
  first.Process({p::CompanionSocialAction::kMessage, 23, {}, u"failure"});
  check(p::CompanionSocialRosterNotification::Decode(a.back()[0]).status ==
        p::CompanionSocialStatus::kUnavailable);
  check(state->messages[23].empty());
  state->fail_queue = false;
  state->fail_read = true;
  first.Process({p::CompanionSocialAction::kMessage,
                 23,
                 {},
                 u"saved despite read failure"});
  check(a.back()[0].message_id == 65015 && state->messages[23].size() == 1);
  check(p::CompanionMailboxNotification::Decode(b.back().back()).status ==
        p::CompanionMailboxStatus::kUnavailable);
  state->fail_read = false;
  for (int i = 0; i < 21; ++i)
    first.Process({p::CompanionSocialAction::kMessage, 23, {}, u"paged"});
  check(state->messages[23].size() == 22);
  second.ProcessMailbox({});
  received = p::CompanionMailboxNotification::Decode(b.back().back());
  check(received.messages.size() == 20);
  const auto batch_count = b.size();
  for (std::size_t i = 0; i < 20; ++i) {
    second.ProcessMailbox({p::CompanionMailboxAction::kAcknowledge,
                           received.messages[i].message_id});
    check(b.size() == batch_count + (i == 19 ? 1 : 0));
  }
  check(p::CompanionMailboxNotification::Decode(b.back().back())
            .messages.size() == 2);
  state->wrong_owner = true;
  second.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(b.back().back()).status ==
        p::CompanionMailboxStatus::kUnavailable);
  state->wrong_owner = false;
  state->invalid_sender = true;
  second.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(b.back().back()).status ==
        p::CompanionMailboxStatus::kUnavailable);
  state->invalid_sender = false;
  first.Release();
  second.Release();
  registry.reset();
  registry = create();
  second = enter_b();
  second.ProcessMailbox({});
  check(p::CompanionMailboxNotification::Decode(b.back().back())
            .messages.size() == 2);
  // A failed sink must retain durable records and permit explicit retry.
  second.Release();
  auto failed_sink = registry->Enter(bob, [](Batch) {
    throw std::runtime_error("Injected transport failure");
  });
  failed_sink.ProcessMailbox({});
  failed_sink.Release();
  check(state->messages[23].size() == 2);
  second = enter_b();
  second.ProcessMailbox({});
  received = p::CompanionMailboxNotification::Decode(b.back().back());
  for (const auto& message : received.messages)
    second.ProcessMailbox(
        {p::CompanionMailboxAction::kAcknowledge, message.message_id});
  check(state->messages[23].empty());
  rejects([&] { failed_sink.ProcessMailbox({}); });
  std::cout << "Offline message service: " << checks << " checks passed.\n";
  return checks;
}
