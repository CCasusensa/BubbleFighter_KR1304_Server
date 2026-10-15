#include <array>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>

#include "features/inventory/default_inventory.h"
#include "features/shop/cash_refund.h"
#include "features/shop/cash_shop.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/shop/cash_balance_response.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace {
namespace bf = bubble_fighter::server;
namespace inv = bf::features::inventory;
namespace shop = bf::features::shop;
namespace p = bf::protocol;
using Character = bf::features::characters::CharacterRecord;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Cash refund service line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
struct Characters final : bf::features::characters::CharacterStore {
  Character value{7, 3, u"RefundTest", 10, 7, {100, 0, 10000, 2000000}};
  std::optional<Character> FindByAccount(std::uint64_t account) override {
    if (account == value.account_id) return value;
    return {};
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(Character&) override {
    return bf::features::characters::CharacterCreateResult::
        kAccountHasCharacter;
  }
};
}  // namespace

int RunCashRefundServiceTests() {
  checks = 0;
  const Character character = Characters{}.value;
  constexpr std::uint64_t now = 1800000000;
  const auto inventory =
      inv::DefaultInventory::Prepare(character, {3, 7, {}}, now);
  const auto buy = [&](std::uint32_t stock, bool track = true) {
    auto result = shop::CashShop::Purchase({stock, 0}, character, inventory,
                                           {3, 7, {}}, 100, now, track);
    Check(result.purchase.response.result == 1);
    return result;
  };
  for (const auto stock : {1000139U, 1000127U, 1000216U, 1005432U, 1010816U}) {
    const auto paid = buy(stock);
    const auto& receipt = paid.pending.purchases.front();
    const auto visible = paid.pending.CreateNotification(3, now);
    Check(visible.slots.size() == receipt.slot_count);
    for (const auto& row : visible.slots) Check(row.byte_at48 == 1);
    const auto refunded = shop::PrepareCashRefund(
        paid.purchase.character, paid.pending,
        {{}, paid.pending.slots.back().slot_id}, now + 1);
    Check(refunded.response.result == 1 && refunded.character == character);
    Check(refunded.cash_returned == receipt.cash_price);
    Check(refunded.pending.slots.empty() && refunded.pending.purchases.empty());
    Check(refunded.removed_slot_ids.size() == receipt.slot_count);
    Check(refunded.packets.size() == 4);
    const std::array<std::uint16_t, 4> expected{105, 834, 824, 810};
    for (std::size_t i = 0; i < expected.size(); ++i)
      Check(refunded.packets[i].message_id == expected[i]);
    Check(p::CashBalanceResponse::Decode(refunded.packets[1]).cash ==
          character.statistics.cash);
    const auto duplicate = shop::PrepareCashRefund(
        refunded.character, refunded.pending, {{}, 100}, now + 2);
    Check(duplicate.response.result == 2 && duplicate.character == character);
  }
  const auto paid = buy(1010816);
  Check(paid.pending.slots.size() == 2);
  Check(paid.pending.slots[0].amount == 65535 &&
        paid.pending.slots[1].amount == 34465);
  for (const auto picked_slot : {100U, 101U}) {
    const auto picked =
        shop::CashShop::Pickup({picked_slot}, paid.purchase.character,
                               inventory, paid.pending, now + 1);
    Check(picked.response.result == 1 &&
          picked.pending.purchases.front().claimed);
    Check(picked.pending.CreateNotification(3, now + 2).slots[0].byte_at48 ==
          0);
    const auto denied = shop::PrepareCashRefund(
        picked.character, picked.pending,
        {{}, picked_slot == 100 ? 101U : 100U}, now + 2);
    Check(denied.response.result == 2 && denied.character == picked.character);
    Check(denied.pending == picked.pending);
  }
  for (const auto offset : {0ULL, 604799ULL, 604800ULL, 604801ULL}) {
    const auto refunded = shop::PrepareCashRefund(
        paid.purchase.character, paid.pending, {{}, 100}, now + offset);
    Check(refunded.response.result == (offset < 604800 ? 1U : 4U));
    Check(paid.pending.CreateNotification(3, now + offset).slots[0].byte_at48 ==
          (offset < 604800 ? 1 : 0));
  }
  const auto before_time = shop::PrepareCashRefund(
      paid.purchase.character, paid.pending, {{}, 100}, now - 1);
  Check(before_time.response.result == 2);
  Check(paid.pending.CreateNotification(3, 0).slots[0].byte_at48 == 0);
  for (const p::CashRefundRequest request :
       {p::CashRefundRequest{{}, 0}, {{}, 999}, {u"credential", 100}}) {
    const auto denied = shop::PrepareCashRefund(paid.purchase.character,
                                                paid.pending, request, now);
    Check(denied.response.result == 2 && denied.pending == paid.pending);
    Check(denied.character == paid.purchase.character &&
          denied.packets.size() == 2);
  }
  auto too_rich = paid.purchase.character;
  too_rich.statistics.cash = 0x7FFFFFFF;
  Check(shop::PrepareCashRefund(too_rich, paid.pending, {{}, 100}, now)
            .response.result == 2);
  auto wrong_owner = paid.pending;
  wrong_owner.account_id = 99;
  Rejects([&] {
    shop::PrepareCashRefund(paid.purchase.character, wrong_owner, {{}, 100},
                            now);
  });
  auto missing = paid.pending;
  missing.slots.pop_back();
  Rejects([&] {
    shop::PrepareCashRefund(paid.purchase.character, missing, {{}, 100}, now);
  });
  auto bad_price = paid.pending;
  --bad_price.purchases[0].cash_price;
  Rejects([&] {
    shop::PrepareCashRefund(paid.purchase.character, bad_price, {{}, 100}, now);
  });
  auto duplicate_receipt = paid.pending;
  duplicate_receipt.purchases.push_back(duplicate_receipt.purchases[0]);
  Rejects([&] { duplicate_receipt.CreateNotification(3, now); });
  const auto legacy = buy(1010816, false);
  Check(legacy.pending.purchases.empty());
  Check(shop::PrepareCashRefund(legacy.purchase.character, legacy.pending,
                                {{}, 100}, now)
            .response.result == 2);
  Check(shop::CashShop::Pickup({101}, legacy.purchase.character, inventory,
                               legacy.pending, now)
            .response.result == 1);

  using Player = bf::sessions::PlayerSession;
  auto characters = std::make_shared<Characters>();
  int calls = 0;
  bool changed_identity = false;
  const auto session = [&](bool with_provider = true) {
    Player::CashRefundHandler handler;
    if (with_provider)
      handler = [&](const auto& owner, const auto& request, auto observed) {
        ++calls;
        Check(owner.account_id == 3 && owner.character_id == 7 &&
              request.slot_id == 100);
        Check(observed > 0);
        auto result =
            shop::PrepareCashRefund(owner, {3, 7, {}}, request, observed);
        if (changed_identity) result.character.account_id = 9;
        return result;
      };
    return Player(
        [](std::string_view) { return std::optional<std::uint64_t>{3}; },
        characters,
        [inventory](const auto& owner, auto observed) {
          return bf::sessions::CreateMenuSnapshot(owner, inventory, observed);
        },
        {}, {}, {}, {}, {}, {},
        bf::features::practice::PracticeSession::Clock::now,
        bf::features::practice::PracticeSession::CreateRandomSeed, {}, {}, {},
        {}, {}, {}, {}, {}, {}, {}, {}, std::move(handler));
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
  Rejects([&] { fresh.Process(p::CashRefundRequest{{}, 100}.Encode()); });
  Check(calls == 0);
  auto pending = session();
  login(pending, false);
  Rejects([&] { pending.Process(p::CashRefundRequest{{}, 100}.Encode()); });
  Check(calls == 0);
  auto absent = session(false);
  login(absent, true);
  Rejects([&] { absent.Process(p::CashRefundRequest{{}, 100}.Encode()); });
  Check(calls == 0);
  auto active = session();
  login(active, true);
  const auto packets = active.Process(p::CashRefundRequest{{}, 100}.Encode());
  Check(packets.size() == 2 && packets[0].message_id == 824 &&
        packets[1].message_id == 810);
  Check(calls == 1);
  changed_identity = true;
  Rejects([&] { active.Process(p::CashRefundRequest{{}, 100}.Encode()); });
  Check(calls == 2);
  std::cout << "Cash refund service: " << checks << " checks passed.\n";
  return checks;
}
