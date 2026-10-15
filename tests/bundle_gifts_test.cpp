#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <source_location>
#include <stdexcept>
#include <tuple>

#include "features/gifts/gift_service.h"
#include "features/inventory/default_inventory.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/stock_set_catalog.h"

namespace {
namespace bf = bubble_fighter::server;
namespace gifts = bf::features::gifts;
namespace inv = bf::features::inventory;
namespace shop = bf::features::shop;
namespace p = bf::protocol;
int checks = 0;
void Check(bool value,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value)
    throw std::runtime_error("Bundle gifts line " + std::to_string(at.line()));
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
const inv::InventorySlot* Find(const inv::CharacterInventory& inventory,
                               std::uint32_t id) {
  const auto it =
      std::find_if(inventory.slots.begin(), inventory.slots.end(),
                   [id](const auto& slot) { return slot.item_id == id; });
  return it == inventory.slots.end() ? nullptr : &*it;
}
}  // namespace

int RunBundleGiftTests(const std::filesystem::path& directory) {
  checks = 0;
  std::ifstream file(directory / "bundle_gifts_cases.json");
  Check(file.good());
  const std::string fixture{std::istreambuf_iterator<char>{file}, {}};
  for (const auto& [key, allowed, count] :
       {std::tuple{"\"allowed\"", true, 391},
        std::tuple{"\"denied_published\"", false, 95}}) {
    const auto start = fixture.find('[', fixture.find(key));
    const auto end = fixture.find(']', start);
    Check(start != std::string::npos && end != std::string::npos);
    const auto values = fixture.substr(start, end - start);
    const std::regex number{"[0-9]+"};
    int found = 0;
    for (auto it = std::sregex_iterator(values.begin(), values.end(), number);
         it != std::sregex_iterator{}; ++it) {
      Check(gifts::PrepareGiftBundle(
                static_cast<std::uint32_t>(std::stoul(it->str())))
                .has_value() == allowed);
      ++found;
    }
    Check(found == count);
  }
  const bf::features::characters::CharacterRecord sender{
      7, 3, u"GiftSender", 10, 7, {100, 0, 10000, 2000000}};
  const bf::features::characters::CharacterRecord recipient{
      8, 4, u"GiftReceiver", 10, 7, {100, 0, 10000, 2000000}};
  constexpr std::uint64_t now = 1800000000;
  const auto inventory =
      inv::DefaultInventory::Prepare(recipient, {4, 8, {}}, now);
  const inv::CashInventory cash{4, 8, {}};
  const auto send = [&](std::uint32_t id, const auto& target, const auto& owned,
                        const auto& pending,
                        std::span<const gifts::GiftRecord> rows = {}) {
    return gifts::PrepareGiftSend({target.character_name, {}, id, u"Bundle"},
                                  sender, target, owned, pending, rows, 0, 37,
                                  now);
  };
  auto sent = send(2000001, recipient, inventory, cash);
  Check(sent.changed && sent.changed->bundle && !sent.inventory);
  Check(sent.changed->bundle->bonus_lucci == 60000 &&
        sent.changed->bundle->components.size() == 1 &&
        sent.changed->bundle->components.front().item_id == 100081);
  Check(sent.character.statistics.cash == 1995500 &&
        sent.character.statistics.lucci == sender.statistics.lucci);
  Check(sent.packets.size() == 3 && sent.packets[0].message_id == 105 &&
        sent.packets[1].message_id == 834 && sent.packets[2].message_id == 825);
  std::array rows{*sent.changed};
  const auto action = [&](const gifts::GiftRequest& request,
                          std::uint64_t time = 1800000010) {
    return gifts::PrepareGiftAction(recipient, inventory, rows, request, time);
  };
  auto received = p::GiftReceivedListResponse::Decode(
      action(p::GiftReceivedListRequest{0, 4}).packets[0]);
  Check(received.records.size() == 1 &&
        received.records[0].stock_id == 2000001 &&
        received.records[0].short_at48 == 0);
  auto claim = action(p::GiftReceiveRequest{37});
  Check(claim.changed && claim.inventory &&
        claim.character.statistics.lucci == 70000);
  Check(claim.packets.size() == 4 && claim.packets[0].message_id == 105 &&
        claim.packets[1].message_id == 802 &&
        claim.packets[2].message_id == 806 &&
        p::GiftReceiveResponse::Decode(claim.packets[3]) ==
            p::GiftReceiveResponse{0, 37, 2000001, 0});
  Check(Find(*claim.inventory, 100081)->quantity == 100);
  rows[0] = *claim.changed;
  Check(!action(p::GiftReceiveRequest{37}).changed &&
        !action(p::GiftDiscardRequest{37}).changed);
  Check(action(p::GiftSendThanksRequest{37, u"Thank you", 4})
            .changed.has_value());
  rows[0] = *sent.changed;
  Check(action(p::GiftDiscardRequest{37}).changed->state ==
        gifts::GiftState::kDiscarded);
  Check(!action(p::GiftReceiveRequest{37}, now + gifts::kGiftLifetimeSeconds)
             .changed);
  Check(!gifts::PrepareGiftAction(sender, {3, 7, {}}, rows,
                                  p::GiftReceiveRequest{37}, now)
             .changed);
  auto full_balance = recipient;
  full_balance.statistics.lucci = 2147483647;
  Check(!send(2000001, full_balance, inventory, cash).changed);
  Check(!gifts::PrepareGiftAction(full_balance, inventory, rows,
                                  p::GiftReceiveRequest{37}, now)
             .changed);
  Check(!send(2000001, recipient, inventory, cash, rows).changed);
  auto ordinary = rows[0];
  ordinary.bundle.reset();
  Rejects([&] { gifts::ValidateGift(ordinary); });
  auto invalid = rows[0];
  invalid.bundle->components.push_back(invalid.bundle->components.front());
  Rejects([&] { gifts::ValidateGift(invalid); });
  invalid = rows[0];
  invalid.bundle->components.front().item_id = 300010;
  Rejects([&] { gifts::ValidateGift(invalid); });
  invalid = rows[0];
  invalid.amount = 2;
  Rejects([&] { gifts::ValidateGift(invalid); });
  // Claim saved immutable terms rather than repricing current resources.
  rows[0].price = 1;
  rows[0].bundle->bonus_lucci = 1234;
  rows[0].bundle->components[0].amount = 1;
  claim = action(p::GiftReceiveRequest{37});
  Check(claim.changed && claim.character.statistics.lucci == 11234 &&
        Find(*claim.inventory, 100081)->quantity == 1);
  for (const auto id : {2000105U, 2000107U}) {
    sent = send(id, recipient, inventory, cash);
    Check(sent.changed && sent.changed->bundle);
    Check(sent.changed->bundle->components.size() == (id == 2000105 ? 4U : 8U));
    Check(sent.character.statistics.cash ==
              (id == 2000105 ? 1995600U : 2000000U) &&
          sent.character.statistics.lucci == (id == 2000107 ? 5500U : 10000U));
    rows[0] = *sent.changed;
    claim = action(p::GiftReceiveRequest{37});
    Check(claim.changed && claim.inventory && claim.packets.size() == 3);
    for (const auto& part : sent.changed->bundle->components) {
      const auto* item = Find(*claim.inventory, part.item_id);
      Check(item && item->expires_at_unix_seconds ==
                        (part.amount == 0xffffffffU
                             ? std::optional<std::uint64_t>{}
                             : std::optional<std::uint64_t>{
                                   now + 10 + part.amount * 86400ULL}));
    }
    auto conflict = inventory;
    const auto& part = sent.changed->bundle->components.back();
    Check(inv::InventoryGrant::Apply(conflict, part.item_id, 0xffffffffU,
                                     part.max_amount, now) ==
          inv::InventoryGrantStatus::kGranted);
    const auto failed = gifts::PrepareGiftAction(
        recipient, conflict, rows, p::GiftReceiveRequest{37}, now);
    Check(!failed.changed && !failed.inventory &&
          failed.character == recipient);
  }
  for (const auto id : {2000003U, 2000045U, 2000812U, 2000813U, 2001722U,
                        2003269U, 2004901U, 2100001U})
    Check(!send(id, recipient, inventory, cash).changed);
  // The same 1024-slot bound covers ordinary gifts and projections containing
  // pending bundle parts. Capacity refusal must not throw or consume the gift.
  inv::CharacterInventory full{
      recipient.account_id, recipient.character_id, {}};
  for (const auto& definition : inv::ItemCatalog::Entries()) {
    if (definition.usage != inv::InventoryUsage::kDuration) continue;
    inv::InventorySlot slot;
    slot.item_id = definition.item_id;
    slot.usage = definition.usage;
    slot.quantity = 1;
    slot.expires_at_unix_seconds = now + 86400;
    full.slots.push_back(slot);
    if (full.slots.size() == 1024) break;
  }
  Check(full.slots.size() == 1024);
  for (const auto id : {2000001U, 1000139U}) {
    Check(!send(id, recipient, full, cash).changed);
    rows[0] = *send(id, recipient, inventory, cash).changed;
    const auto refused = gifts::PrepareGiftAction(
        recipient, full, rows, p::GiftReceiveRequest{37}, now);
    Check(!refused.changed && !refused.inventory &&
          refused.character == recipient);
  }
  rows[0] = *send(1000139, recipient, inventory, cash).changed;
  Check(!send(2000001, recipient, full, cash, rows).changed);
  std::cout << "Bundle gifts: " << checks
            << " checks passed; 391 eligible, 95 published refusals.\n";
  return checks;
}
