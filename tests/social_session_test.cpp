#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/scenes/chat_request.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace {
struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record{17, 7, u"Alice", 27, 2};
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    return id == 7 ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected social character creation");
  }
};
struct MailboxState {
  std::vector<proto::CompanionQueuedMessage> messages;
  std::vector<std::uint64_t> acknowledgements;
};
struct Social final : bf::features::social::SocialStore {
  explicit Social(std::shared_ptr<MailboxState> state)
      : mailbox(std::move(state)) {}
  std::shared_ptr<MailboxState> mailbox;
  bf::features::social::SocialMailboxSnapshot LoadMailbox(
      const bf::features::characters::CharacterRecord& owner,
      std::uint64_t) override {
    auto snapshot = Load(owner);
    if (!mailbox->messages.empty())
      snapshot.rows.push_back({23, u"Bob", 1, false});
    return {proto::CompanionMailboxStatus::kOk, std::move(snapshot),
            mailbox->messages};
  }
  void AcknowledgeMessage(const bf::features::characters::CharacterRecord&,
                          std::uint64_t id) override {
    mailbox->acknowledgements.push_back(id);
    std::erase_if(mailbox->messages, [id](const auto& message) {
      return message.message_id == id;
    });
  }
  bf::features::social::SocialSnapshot Load(
      const bf::features::characters::CharacterRecord& owner) override {
    return {owner, {}, {}};
  }
  std::optional<proto::ChatBlockEntry> Find(std::uint32_t,
                                            std::u16string_view) override {
    return {};
  }
  bf::features::social::SocialChange Change(
      const bf::features::characters::CharacterRecord&,
      const proto::CompanionSocialRequest&) override {
    throw std::logic_error("Unexpected social change");
  }
  proto::CompanionSocialStatus CanMessage(
      const bf::features::characters::CharacterRecord&,
      std::uint32_t) override {
    return proto::CompanionSocialStatus::kNotFriend;
  }
};
}  // namespace

int RunSocialSessionTests() {
  int checks = 0;
  const auto check = [&](bool ok, std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok)
      throw std::runtime_error("Social session line " +
                               std::to_string(at.line()));
  };
  const auto rejects = [&](auto action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected);
  };
  using Session = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  auto now = Practice::Clock::now();
  auto mailbox = std::make_shared<MailboxState>();
  auto registry = std::make_shared<bf::features::social::SocialRegistry>(
      [mailbox] { return std::make_unique<Social>(mailbox); });
  const auto player = [&] {
    return std::make_unique<Session>(
        [](std::string_view) { return std::optional<std::uint64_t>{7}; },
        std::make_shared<Characters>(),
        [](const auto& member, auto observed) {
          return bf::sessions::CreateMenuSnapshot(
              member, {member.account_id, member.character_id, {}}, observed);
        },
        Session::EquipmentChangeHandler{}, Session::ShopCatalogProvider{},
        Session::ShopPurchaseHandler{}, Session::CashPickupHandler{},
        Session::WeaponPresetHandler{}, Session::ItemUseHandler{},
        [&] { return now; }, Practice::CreateRandomSeed, nullptr, nullptr,
        registry);
  };
  const auto login = [](Session& session) {
    proto::ClientToServerMessage0x0001 hello;
    hello.initial_value = 1256;
    session.Process(hello.Encode());
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    session.Process(login.Encode());
  };
  auto mailbox_premature = player();
  rejects([&] {
    mailbox_premature->Process(proto::CompanionMailboxRequest{}.Encode());
  });
  check(mailbox_premature->State() ==
        bf::sessions::PlayerSessionState::kRejected);
  auto mailbox_uninitialized = player();
  login(*mailbox_uninitialized);
  rejects([&] {
    mailbox_uninitialized->Process(proto::CompanionMailboxRequest{}.Encode());
  });
  auto premature = player();
  auto group_premature = player();
  rejects([&] {
    group_premature->Process(proto::CompanionFriendGroupRequest{}.Encode());
  });
  check(group_premature->State() ==
        bf::sessions::PlayerSessionState::kRejected);
  rejects(
      [&] { premature->Process(proto::CompanionSocialRequest{}.Encode()); });
  check(premature->State() == bf::sessions::PlayerSessionState::kRejected);
  auto uninitialized = player();
  auto group_uninitialized = player();
  login(*group_uninitialized);
  rejects([&] {
    group_uninitialized->Process(proto::CompanionFriendGroupRequest{}.Encode());
  });
  login(*uninitialized);
  rejects([&] {
    uninitialized->Process(proto::ChatBlockRequest{false, {}, u"Bob"}.Encode());
  });
  auto session = player();
  login(*session);
  const auto initialized =
      session->Process(proto::ClientToServerMessage0x0209{}.Encode());
  check(initialized.back().message_id == 170);
  check(proto::ChatBlockListNotification::Decode(initialized.back())
            .entries.empty());
  check(
      session->Process(proto::ClientToServerMessage0x0209{}.Encode()).empty());
  // Scene chat and social requests share five actions in a three-second window.
  for (int i = 0; i < 4; ++i) {
    const auto packets =
        session->Process(proto::CompanionSocialRequest{}.Encode());
    check(packets.size() == 1 &&
          proto::CompanionSocialRosterNotification::Decode(packets[0]).status ==
              proto::CompanionSocialStatus::kOk);
  }
  check(session->Process(proto::ChatRequest{0, u"hello", {}}.Encode())
            .at(0)
            .message_id == 509);
  auto limited = session->Process(proto::CompanionSocialRequest{}.Encode());
  const auto group_limited =
      session->Process(proto::CompanionFriendGroupRequest{}.Encode());
  check(group_limited.size() == 1 &&
        proto::CompanionFriendGroupsNotification::Decode(group_limited[0])
                .status == proto::CompanionFriendGroupStatus::kInvalid);
  check(
      proto::CompanionSocialRosterNotification::Decode(limited.at(0)).status ==
      proto::CompanionSocialStatus::kInvalid);
  check(
      session->Process(proto::ChatRequest{0, u"limited", {}}.Encode()).empty());
  now += std::chrono::seconds(4);
  check(proto::CompanionSocialRosterNotification::Decode(
            session->Process(proto::CompanionSocialRequest{}.Encode()).at(0))
            .status == proto::CompanionSocialStatus::kOk);
  check(session->State() ==
        bf::sessions::PlayerSessionState::kMenuInitializationSent);
  const auto groups =
      session->Process(proto::CompanionFriendGroupRequest{}.Encode());
  check(groups.size() == 2 && groups[0].message_id == 65011 &&
        groups[1].message_id == 65013);
  check(proto::CompanionFriendGroupsNotification::Decode(groups[1]).status ==
        proto::CompanionFriendGroupStatus::kUnavailable);
  // Once explicitly subscribed, roster updates always include the group view.
  const auto paired =
      session->Process(proto::CompanionSocialRequest{}.Encode());
  check(paired.size() == 2 && paired[0].message_id == 65011 &&
        paired[1].message_id == 65013);
  session.reset();
  auto reconnect = player();
  login(*reconnect);
  check(reconnect->Process(proto::ClientToServerMessage0x0209{}.Encode())
            .back()
            .message_id == 170);
  for (std::uint64_t i = 1; i <= 20; ++i)
    mailbox->messages.push_back({i, 23, 1792000000, u"Bob", u"queued"});
  auto deliveries =
      reconnect->Process(proto::CompanionMailboxRequest{}.Encode());
  check(proto::CompanionMailboxNotification::Decode(deliveries.back())
            .messages.size() == 20);
  for (int i = 0; i < 4; ++i)
    reconnect->Process(proto::CompanionSocialRequest{}.Encode());
  auto mailbox_limited =
      reconnect->Process(proto::CompanionMailboxRequest{}.Encode());
  check(proto::CompanionMailboxNotification::Decode(mailbox_limited.back())
            .status == proto::CompanionMailboxStatus::kRateLimited);
  for (std::uint64_t i = 1; i <= 20; ++i) {
    auto response = reconnect->Process(proto::CompanionMailboxRequest{
        proto::CompanionMailboxAction::kAcknowledge, i}
                                           .Encode());
    check(mailbox->acknowledgements.size() == i);
    if (i < 20)
      check(response.empty());
    else
      check(proto::CompanionMailboxNotification::Decode(response.back())
                .messages.empty());
  }
  check(reconnect
            ->Process(proto::CompanionMailboxRequest{
                proto::CompanionMailboxAction::kAcknowledge, 20}
                          .Encode())
            .empty());
  rejects([&] {
    reconnect->Process(proto::CompanionMailboxRequest{
        proto::CompanionMailboxAction::kAcknowledge, 21}
                           .Encode());
  });
  check(reconnect->State() == bf::sessions::PlayerSessionState::kRejected);
  std::cout << checks << " social authentication and quota checks passed\n";
  return checks;
}
