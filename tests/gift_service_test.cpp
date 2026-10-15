#include "features/gifts/gift_service.h"

#include <array>
#include <bit>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>

#include "features/inventory/default_inventory.h"
#include "features/shop/cash_shop.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace inv = bf::features::inventory;
namespace gifts = bf::features::gifts;
namespace p = bf::protocol;
using Character = bf::features::characters::CharacterRecord;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Gift service line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool refused = false;
  try {
    action();
  } catch (const std::exception&) {
    refused = true;
  }
  Check(refused);
}
struct Characters final : bf::features::characters::CharacterStore {
  Character value{7, 3, u"GiftSender", 10, 7, {100, 0, 10000, 2000000}};
  std::optional<Character> FindByAccount(std::uint64_t account) override {
    return account == value.account_id ? std::optional{value} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(Character&) override {
    return bf::features::characters::CharacterCreateResult::
        kAccountHasCharacter;
  }
};
}  // namespace
int RunGiftServiceTests() {
  checks = 0;
  const Character sender = Characters{}.value;
  const Character recipient{8,  4, u"GiftReceiver",
                            10, 7, {100, 0, 10000, 2000000}};
  constexpr std::uint64_t now = 1800000000;
  const auto inventory =
      inv::DefaultInventory::Prepare(recipient, {4, 8, {}}, now);
  const auto sender_inventory =
      inv::DefaultInventory::Prepare(sender, {3, 7, {}}, now);
  const inv::CashInventory cash{4, 8, {}};
  const auto send = [&](std::uint32_t stock,
                        std::span<const gifts::GiftRecord> rows = {}) {
    return gifts::PrepareGiftSend(
        {recipient.character_name, {}, stock, u"Enjoy"}, sender, recipient,
        inventory, cash, rows, 0, 10, now);
  };
  const auto action = [&](const gifts::GiftRequest& request,
                          std::span<const gifts::GiftRecord> rows,
                          std::uint64_t time = 1800000000) {
    return gifts::PrepareGiftAction(recipient, inventory, rows, request, time);
  };
  // Fixed resource terms, independent of the implementation's computed result.
  const std::array stocks{1000139U, 1000216U, 1005432U, 1010816U};
  const std::array items{205034U, 100072U, 207925U, 102613U};
  const std::array amounts{100U, 0xffffffffU, 100U, 100000U};
  const std::array prices{1000U, 1U, 1000U, 999999U};
  for (std::size_t i = 0; i < stocks.size(); ++i) {
    const auto sent = send(stocks[i]);
    Check(sent.changed.has_value() && !sent.inventory);
    Check(sent.changed->item_id == items[i] &&
          sent.changed->amount == amounts[i] &&
          sent.changed->price == prices[i] && sent.changed->cash);
    Check(sent.character.statistics.cash == 2000000U - prices[i]);
    Check(sent.character.statistics.lucci == 10000 && sent.packets.size() == 3);
    Check(sent.packets[0].message_id == 105 &&
          sent.packets[1].message_id == 834 &&
          p::GiftSendResponse::Decode(sent.packets[2]).result == 1);
    const std::array rows{*sent.changed};
    const auto claimed = action(p::GiftReceiveRequest{10}, rows, now + 1);
    Check(claimed.changed->state == gifts::GiftState::kClaimed &&
          claimed.inventory);
    Check(claimed.packets.size() == 3 && claimed.packets[0].message_id == 802 &&
          claimed.packets[1].message_id == 806);
    const auto reply = p::GiftReceiveResponse::Decode(claimed.packets.back());
    Check(reply.result == 0 && reply.gift_id == 10 &&
          reply.value_at8 == stocks[i] && reply.new_count == 0);
    const std::array claimed_rows{*claimed.changed};
    Check(!action(p::GiftReceiveRequest{10}, claimed_rows, now + 2).changed);
    Check(!action(p::GiftDiscardRequest{10}, claimed_rows, now + 2).changed);
    const auto thanks = gifts::PrepareGiftAction(
        recipient, *claimed.inventory, claimed_rows,
        p::GiftSendThanksRequest{10, u"Thank you", 2}, now + 2);
    Check(thanks.changed->thanks_emotion == 2 &&
          thanks.changed->thanks_color == 7);
    Check(thanks.changed->thanks_appearance.value_at0 == 300010);
    const std::array thanked{*thanks.changed};
    Check(!action(p::GiftSendThanksRequest{10, u"Again", 3}, thanked, now + 3)
               .changed);
    const auto view =
        gifts::PrepareGiftAction(sender, sender_inventory, thanked,
                                 p::GiftViewThanksRequest{10}, now + 3);
    const auto message = p::GiftViewThanksResponse::Decode(view.packets[0]);
    Check(message.value_at0 == 1 &&
          message.text_at8 == recipient.character_name &&
          message.text_at32 == u"Thank you" && message.value_at56 == 2 &&
          message.value_at60 == 7);
    Check(p::GiftViewThanksResponse::Decode(
              action(p::GiftViewThanksRequest{10}, thanked, now + 3).packets[0])
              .value_at0 == 0);
    const auto hidden =
        action(p::GiftDeleteReceivedRequest{10}, thanked, now + 3);
    Check(hidden.changed->recipient_hidden && !hidden.changed->sender_hidden);
    const std::array hidden_rows{*hidden.changed};
    Check(p::GiftViewThanksResponse::Decode(
              gifts::PrepareGiftAction(sender, sender_inventory, hidden_rows,
                                       p::GiftViewThanksRequest{10}, now + 3)
                  .packets[0])
              .value_at0 == 1);
  }
  auto gift = *send(1000139).changed;
  std::array rows{gift};
  const auto received = p::GiftReceivedListResponse::Decode(
      action(p::GiftReceivedListRequest{0, 4}, rows).packets[0]);
  Check(received.result == 1 && received.total_count == 1 &&
        received.value_at8 == 1);
  const auto& record = received.records[0];
  Check(record.value_at16 == 7 && record.text_at20 == u"GiftSender" &&
        record.text_at52 == u"Enjoy" && record.short_at48 == 0);
  Check(std::bit_cast<double>(record.wide_at8) ==
        25569.0 + double(now) / 86400.0);
  Check(std::bit_cast<double>(record.wide_at80) ==
        25569.0 + double(now + 2592000) / 86400.0);
  const auto sent_list = p::GiftSentListResponse::Decode(
      gifts::PrepareGiftAction(sender, sender_inventory, rows,
                               p::GiftSentListRequest{0, 4}, now)
          .packets[0]);
  Check(sent_list.records[0].text_at28 == u"GiftReceiver" &&
        sent_list.records[0].short_at58 == 1 &&
        sent_list.records[0].wide_at16 == 0);
  Check(!action(p::GiftDeleteReceivedRequest{10}, rows).changed);
  Check(!action(p::GiftSendThanksRequest{10, u"Too early", 2}, rows).changed);
  Check(!action(p::GiftDeleteSentRequest{10}, rows).changed);
  auto sender_deleted = gifts::PrepareGiftAction(
      sender, sender_inventory, rows, p::GiftDeleteSentRequest{10}, now);
  Check(sender_deleted.changed->sender_hidden);
  rows[0] = *sender_deleted.changed;
  Check(action(p::GiftReceiveRequest{10}, rows, now + 1).changed.has_value());
  rows[0] = gift;
  for (auto offset : {2591999ULL, 2592000ULL, 2592001ULL}) {
    const auto time = now + offset;
    Check(action(p::GiftReceiveRequest{10}, rows, time).changed.has_value() ==
          (offset < 2592000));
    Check(gifts::PendingGiftCount(8, rows, time) ==
          (offset < 2592000 ? 1U : 0U));
    Check(p::GiftReceivedListResponse::Decode(
              action(p::GiftReceivedListRequest{0, 4}, rows, time).packets[0])
              .records[0]
              .short_at48 == (offset < 2592000 ? 0 : 3));
  }
  Check(action(p::GiftDeleteReceivedRequest{10}, rows, now + 2592000)
            .changed.has_value());
  const auto discarded = action(p::GiftDiscardRequest{10}, rows);
  rows[0] = *discarded.changed;
  Check(rows[0].state == gifts::GiftState::kDiscarded);
  Check(!action(p::GiftReceiveRequest{10}, rows).changed &&
        !action(p::GiftDiscardRequest{10}, rows).changed);
  Check(!action(p::GiftSendThanksRequest{10, {}, 2}, rows).changed);
  rows[0] = gift;
  for (auto count : {0U, 5U, 0xffffffffU})
    Check(p::GiftReceivedListResponse::Decode(
              action(p::GiftReceivedListRequest{0, count}, rows).packets[0])
              .result == 2);
  Check(p::GiftReceivedListResponse::Decode(
            action(p::GiftReceivedListRequest{0xffffffffU, 4}, rows).packets[0])
            .records.empty());
  auto wrong = gift;
  wrong.recipient_id = 999;
  wrong.sender_id = 998;
  Rejects([&] { action(p::GiftReceiveRequest{10}, std::array{wrong}); });
  Rejects([&] { action(p::GiftReceiveRequest{10}, rows, now - 1); });
  Rejects([&] { action(p::GiftReceiveRequest{10}, std::array{gift, gift}); });
  for (auto corrupt : {0, 1, 2, 3}) {
    wrong = gift;
    if (corrupt == 0) wrong.item_id = 1;
    if (corrupt == 1) wrong.stock_id = 1;
    if (corrupt == 2) wrong.handled_at = now;
    if (corrupt == 3) wrong.thanks_message = u"Unowned";
    Rejects([&] { gifts::ValidateGift(wrong); });
  }
  for (auto message : {std::u16string(129, u'a'), std::u16string(u"a\0b", 3),
                       std::u16string(1, 0xd800)}) {
    auto result = gifts::PrepareGiftSend(
        {recipient.character_name, {}, 1000139, message}, sender, recipient,
        inventory, cash, {}, 0, 10, now);
    Check(!result.changed &&
          p::GiftSendResponse::Decode(result.packets[0]).result ==
              std::uint32_t(-76));
  }
  auto poor = sender;
  poor.statistics.cash = 0;
  Check(p::GiftSendResponse::Decode(
            gifts::PrepareGiftSend({recipient.character_name, {}, 1000139, {}},
                                   poor, recipient, inventory, cash, {}, 0, 10,
                                   now)
                .packets[0])
            .result == 27);
  Check(!send(0).changed);
  Check(p::GiftSendResponse::Decode(send(1000127).packets[0]).result ==
        std::uint32_t(-15));
  const auto lucci = send(1000004);
  Check(lucci.changed && !lucci.changed->cash && lucci.changed->price == 60 &&
        lucci.character.statistics.lucci == 9940 &&
        lucci.character.statistics.cash == 2000000);
  Check(lucci.packets.size() == 2 && lucci.packets[0].message_id == 105 &&
        lucci.packets[1].message_id == 825);
  Check(p::GiftSendResponse::Decode(
            gifts::PrepareGiftSend({recipient.character_name, {}, 1000139, {}},
                                   sender, recipient, inventory, cash, {}, 200,
                                   10, now)
                .packets[0])
            .result == 8);
  const auto permanent = send(1000216);
  Check(!send(1000216, std::array{*permanent.changed}).changed);
  const auto pending_cash = bf::features::shop::CashShop::Purchase(
      {1000216, 0}, recipient, inventory, cash, 100, now);
  Check(!gifts::PrepareGiftSend({recipient.character_name, {}, 1000216, {}},
                                sender, recipient, inventory,
                                pending_cash.pending, {}, 0, 10, now)
             .changed);
  auto owned =
      action(p::GiftReceiveRequest{10}, std::array{*permanent.changed});
  Check(!gifts::PrepareGiftSend({recipient.character_name, {}, 1000216, {}},
                                sender, recipient, *owned.inventory, cash, {},
                                0, 10, now)
             .changed);
  for (auto emotion : {0U, 1U, 2U, 6U, 7U, 0xffffffffU}) {
    const auto result = gifts::PrepareGiftAction(
        recipient, *owned.inventory, std::array{*owned.changed},
        p::GiftSendThanksRequest{10, u"Thanks", emotion}, now);
    Check(result.changed.has_value() == (emotion >= 2 && emotion <= 6));
  }
  Check(!gifts::PrepareGiftAction(
             recipient, *owned.inventory, std::array{*owned.changed},
             p::GiftSendThanksRequest{10, std::u16string(25, u'a'), 2}, now)
             .changed);
  std::vector<gifts::GiftRecord> full(200, gift);
  for (std::size_t i = 0; i < full.size(); ++i)
    full[i].gift_id = static_cast<std::uint32_t>(i + 100);
  Check(p::GiftSendResponse::Decode(send(1000139, full).packets[0]).result ==
        8);
  const auto last_page = p::GiftReceivedListResponse::Decode(
      action(p::GiftReceivedListRequest{196, 4}, full).packets[0]);
  Check(last_page.total_count == 200 && last_page.value_at8 == 200 &&
        last_page.records.size() == 4);
  Check(last_page.records.front().gift_id == 103 &&
        last_page.records.back().gift_id == 100);
  full.push_back(gift);
  Rejects([&] { action(p::GiftReceivedListRequest{0, 4}, full); });

  using Player = bf::sessions::PlayerSession;
  auto characters = std::make_shared<Characters>();
  int calls = 0, polls = 0;
  bool wrong_identity = false;
  std::uint32_t pending = 0;
  auto clock = bf::features::practice::PracticeSession::Time{};
  const auto session = [&](bool provider = true) {
    Player::GiftHandler handler;
    if (provider)
      handler = [&](const auto& owner, const auto& request, auto observed) {
        ++calls;
        auto result = gifts::PrepareGiftAction(owner, sender_inventory, {},
                                               request, observed);
        if (wrong_identity) result.character.account_id = 99;
        return result;
      };
    return Player(
        [](std::string_view) { return std::optional<std::uint64_t>{3}; },
        characters,
        [sender_inventory](const auto& owner, auto observed) {
          return bf::sessions::CreateMenuSnapshot(owner, sender_inventory,
                                                  observed);
        },
        {}, {}, {}, {}, {}, {}, [&] { return clock; },
        bf::features::practice::PracticeSession::CreateRandomSeed, {}, {}, {},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, std::move(handler),
        [&](const auto&, auto) {
          ++polls;
          return pending;
        });
  };
  const auto login = [&](auto& player, bool menu) {
    p::ClientToServerMessage0x0001 initial;
    initial.initial_value = 1256;
    player.Process(initial.Encode());
    p::LoginRequest request;
    request.nexon_passport = u"BFS1." + std::u16string(64, u'a');
    player.Process(request.Encode());
    if (menu) player.Process(p::ClientToServerMessage0x0209{}.Encode());
  };
  auto fresh = session();
  Rejects([&] { fresh.Process(p::GiftReceivedListRequest{0, 4}.Encode()); });
  Check(calls == 0 && polls == 0);
  auto early = session();
  login(early, false);
  Rejects([&] { early.Process(p::GiftReceiveRequest{10}.Encode()); });
  Check(calls == 0);
  auto absent = session(false);
  login(absent, true);
  Rejects([&] { absent.Process(p::GiftSentListRequest{0, 4}.Encode()); });
  auto active = session();
  login(active, true);
  Check(active.Poll().empty());
  Check(polls == 1);
  pending = 3;
  Check(active.Poll().empty());
  Check(polls == 1);
  clock += std::chrono::seconds(5);
  Check(p::GiftNewReceiveNotification::Decode(active.Poll()[0]).new_count == 3);
  pending = 0;
  clock += std::chrono::seconds(5);
  Check(p::GiftNewReceiveNotification::Decode(active.Poll()[0]).new_count == 0);
  const std::array<gifts::GiftRequest, 8> requests{
      p::GiftReceivedListRequest{0, 4},    p::GiftSentListRequest{0, 4},
      p::GiftReceiveRequest{10},           p::GiftDiscardRequest{10},
      p::GiftSendThanksRequest{10, {}, 2}, p::GiftViewThanksRequest{10},
      p::GiftDeleteSentRequest{10},        p::GiftDeleteReceivedRequest{10}};
  const std::array<std::uint16_t, 8> response_ids{822, 823, 826, 827,
                                                  828, 829, 831, 832};
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto packet = std::visit(
        [](const auto& request) { return request.Encode(); }, requests[i]);
    Check(active.Process(packet)[0].message_id == response_ids[i]);
  }
  Check(calls == 8);
  wrong_identity = true;
  Rejects([&] { active.Process(p::GiftReceiveRequest{10}.Encode()); });
  auto malformed = session();
  login(malformed, true);
  Rejects([&] { malformed.Process({819, {1, 2, 3}}); });
  std::cout << "Gift service: " << checks << " checks passed.\n";
  return checks;
}
