#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "features/gifts/gift_delivery.h"
#include "features/inventory/item_catalog.h"

int RunDeliveryCapacityTests(const std::filesystem::path& directory) {
  namespace bf = bubble_fighter::server;
  namespace inv = bf::features::inventory;
  int checks = 0;
  const auto check = [&](bool value, std::source_location at =
                                         std::source_location::current()) {
    ++checks;
    if (!value)
      throw std::runtime_error("Delivery capacity line " +
                               std::to_string(at.line()));
  };
  const bf::features::characters::CharacterRecord owner{
      7, 3, u"Capacity", 10, 7, {100, 0, 10000, 2000000}};
  constexpr std::uint64_t now = 1800000000;
  const auto item_for = [&](inv::InventoryUsage usage) {
    const auto found = std::find_if(
        inv::ItemCatalog::Entries().begin(), inv::ItemCatalog::Entries().end(),
        [&](const auto& item) { return item.usage == usage; });
    check(found != inv::ItemCatalog::Entries().end());
    return found->item_id;
  };
  std::ifstream fixture(directory / "delivery_capacity_cases.txt");
  check(fixture.good());
  std::string line;
  int cases = 0;
  while (std::getline(fixture, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream input(line);
    std::string kind, name;
    input >> kind >> name;
    ++cases;
    auto character = owner;
    inv::CharacterInventory inventory{owner.account_id, owner.character_id, {}};
    inv::PendingDelivery delivery;
    bool expected = false;
    if (kind == "item") {
      unsigned usage;
      std::uint32_t quantity, days, first, first_cap, second, second_cap;
      input >> usage >> quantity >> days >> first >> first_cap >> second >>
          second_cap >> expected;
      check(!input.fail());
      const auto type = static_cast<inv::InventoryUsage>(usage);
      const auto item = item_for(type);
      if (quantity) {
        inv::InventorySlot slot;
        slot.item_id = item;
        slot.usage = type;
        slot.quantity = quantity;
        if (type == inv::InventoryUsage::kDuration && days != 0xffffffffU)
          slot.expires_at_unix_seconds = now + std::uint64_t{days} * 86400;
        inventory.slots.push_back(slot);
      }
      delivery.components.push_back({1, item, type, first, first_cap});
      if (second)
        delivery.components.push_back({2, item, type, second, second_cap});
      bool all_orders_deliver = true;
      for (int order = 0; order < 2; ++order) {
        check(
            (inv::CheckDeliveryCapacity(character, inventory, delivery, now) ==
             inv::InventoryGrantStatus::kGranted) == expected);
        auto actual = inventory;
        for (const auto& part : delivery.components)
          if (inv::InventoryGrant::Apply(actual, part.item_id, part.amount,
                                         part.max_amount, now) !=
              inv::InventoryGrantStatus::kGranted) {
            all_orders_deliver = false;
            break;
          }
        std::reverse(delivery.components.begin(), delivery.components.end());
      }
      check(all_orders_deliver == expected);
    } else if (kind == "balance") {
      std::uint64_t first, second;
      input >> character.statistics.lucci >> first >> second >> expected;
      check(!input.fail());
      delivery.bonus_lucci = first + second;
      check((inv::CheckDeliveryCapacity(character, inventory, delivery, now) ==
             inv::InventoryGrantStatus::kGranted) == expected);
    } else if (kind == "slots") {
      std::size_t owned, extra;
      input >> owned >> extra >> expected;
      check(!input.fail());
      for (const auto& item : inv::ItemCatalog::Entries()) {
        if (item.usage != inv::InventoryUsage::kDuration) continue;
        inv::InventorySlot slot;
        slot.item_id = item.item_id;
        slot.usage = item.usage;
        slot.quantity = 1;
        slot.expires_at_unix_seconds = now + 86400;
        inventory.slots.push_back(slot);
        if (inventory.slots.size() == owned) break;
      }
      check(inventory.slots.size() == owned);
      if (extra) {
        for (const auto& item : inv::ItemCatalog::Entries()) {
          if (item.usage != inv::InventoryUsage::kCount) continue;
          delivery.components.push_back({1, item.item_id, item.usage, 1, 100});
          if (delivery.components.size() == extra) break;
        }
      } else {
        const auto& slot = inventory.slots.front();
        delivery.components.push_back({1, slot.item_id, slot.usage, 1, 100});
      }
      check((inv::CheckDeliveryCapacity(character, inventory, delivery, now) ==
             inv::InventoryGrantStatus::kGranted) == expected);
    } else {
      check(false);
    }
    std::string trailing;
    check(!(input >> trailing));
  }
  check(cases == 24);
  const inv::CharacterInventory empty{owner.account_id, owner.character_id, {}};
  for (int invalid = 0; invalid < 5; ++invalid) {
    auto character = owner;
    inv::PendingDelivery delivery;
    delivery.components.push_back({1, item_for(inv::InventoryUsage::kCount),
                                   inv::InventoryUsage::kCount, 1, 100});
    if (invalid == 0) character.account_id = 99;
    if (invalid == 1) delivery.components[0].item_id = 1;
    if (invalid == 2) delivery.components[0].amount = 0;
    if (invalid == 3) delivery.components[0].max_amount = 0;
    bool refused = false;
    try {
      (void)inv::CheckDeliveryCapacity(character, empty, delivery,
                                       invalid == 4 ? 0 : now);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    check(refused);
  }
  std::cout << "Delivery capacity: " << checks
            << " checks passed; 24 independent policy cases.\n";
  return checks;
}
