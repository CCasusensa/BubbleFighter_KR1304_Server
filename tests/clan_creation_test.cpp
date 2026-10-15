#include "features/clans/clan_creation.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>

#include "protocol/inventory/item_use_response.h"
#include "sessions/clan_session.h"

namespace bf = bubble_fighter::server;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan creation line " + std::to_string(at.line()));
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
void Fixtures(const std::filesystem::path& directory) {
  std::ifstream input(directory / "clan_creation_packets.txt");
  Check(input.good());
  std::string name, hex;
  std::uint16_t mid = 0;
  int count = 0;
  while (input >> name >> mid >> hex) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2)
      bytes.push_back(
          static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    const std::uint32_t item = name.starts_with("creation_card") ? 100819
                               : name.starts_with("opaque_item") ? 0x12345678
                                                                 : 0xffffffff;
    if (mid == 1028) {
      const bf::protocol::ClanCreateResponse expected{1, item, 0};
      Check(expected.Encode().payload == bytes);
      Check(bf::protocol::ClanCreateResponse::Decode({mid, bytes}) == expected);
    } else if (mid == 806) {
      const bf::protocol::ItemUseRequest expected{item, 1, {}};
      Check(expected.Encode().payload == bytes);
      Check(bf::protocol::ItemUseRequest::Decode({mid, bytes}) == expected);
    } else {
      Check(mid == 813);
      const bf::protocol::ItemUseResponse expected{0, item, {}};
      Check(expected.Encode().payload == bytes);
      Check(bf::protocol::ItemUseResponse::Decode({mid, bytes}) == expected);
    }
    ++count;
  }
  Check(count == 9);
}
void Preparation() {
  using bf::features::clans::ClanCreation;
  bf::protocol::ClanCreateRequest valid{u"ClanName", 0, u"Hello world", 100819,
                                        30};
  Check(ClanCreation::ValidateRequest(valid) == 1);
  for (const auto size : {0U, 3U, 4U, 12U, 13U}) {
    auto request = valid;
    request.name.assign(size, u'A');
    Check(ClanCreation::ValidateRequest(request) ==
          (size >= 4 && size <= 12 ? 1U : 0xffffff9cU));
  }
  for (const auto size : {0U, 1U, 96U, 97U}) {
    auto request = valid;
    request.introduction.assign(size, u'A');
    Check(ClanCreation::ValidateRequest(request) ==
          (size >= 1 && size <= 96 ? 1U : 0xffffff9bU));
  }
  for (const auto name : {u"名字战队", u"Clan🚀"}) {
    auto request = valid;
    request.name = name;
    Check(ClanCreation::ValidateRequest(request) == 1);
  }
  for (const auto name :
       {u"Bad Name", u"Bad<Clan", u"Bad\nClan", u"Bad\u200bClan"}) {
    auto request = valid;
    request.name = name;
    Check(ClanCreation::ValidateRequest(request) == 0);
  }
  for (const auto unit :
       {char16_t{0xd800}, char16_t{0xdc00}, char16_t{0x200b}, u'<'}) {
    auto request = valid;
    request.introduction.assign(1, unit);
    Check(ClanCreation::ValidateRequest(request) == 0);
  }
  auto request = valid;
  request.introduction = u"A 🚀 B";
  Check(ClanCreation::ValidateRequest(request) == 1);
  request = valid;
  ++request.item_id;
  Check(ClanCreation::ValidateRequest(request) == 0);
  request = valid;
  ++request.item_parameter;
  Check(ClanCreation::ValidateRequest(request) == 0);
  request = valid;
  ++request.value_after_name;
  Check(ClanCreation::ValidateRequest(request) == 0);
  using namespace bf::features::inventory;
  InventorySlot card{100819, InventoryUsage::kCount, 2};
  card.revision = 5;
  card.text_at36 = u"Retained";
  CharacterInventory original{44, 71, {card}};
  for (int failure = 0; failure < 8; ++failure) {
    auto inventory = original;
    auto level = 1U, item = 100819U;
    switch (failure) {
      case 0:
        level = 0;
        break;
      case 1:
        ++item;
        break;
      case 2:
        inventory.slots.clear();
        break;
      case 3:
        inventory.slots[0].quantity = 0;
        break;
      case 4:
        inventory.slots[0].equipped = true;
        break;
      case 5:
        inventory.slots[0].revision = 0;
        break;
      case 6:
        inventory.slots[0].revision =
            (std::numeric_limits<std::uint64_t>::max)();
        break;
      case 7:
        inventory.slots[0].usage = InventoryUsage::kLife;
        break;
    }
    const auto before = inventory.slots;
    Check(!ClanCreation::ConsumeCard(inventory, level, item, 1704067200));
    Check(inventory.slots == before);
  }
  auto inventory = original;
  Check(ClanCreation::ConsumeCard(inventory, 1, 100819, 1704067200));
  Check(inventory.slots[0].quantity == 1 && inventory.slots[0].revision == 5 &&
        inventory.slots[0].text_at36 == u"Retained");
  Check(ClanCreation::ConsumeCard(inventory, 1, 100819, 1704067200));
  Check(inventory.slots[0].quantity == 0 &&
        inventory.ToRecords(44, 1704067200)[0].flags == 2);
  Check(!ClanCreation::ConsumeCard(inventory, 1, 100819, 1704067200));
  inventory = original;
  inventory.slots.push_back(card);
  Rejects([&] { ClanCreation::ConsumeCard(inventory, 1, 100819, 1704067200); });
}
void Session() {
  using namespace bf;
  auto auth = std::make_shared<features::clans::ClanAuthentication>();
  auto game = auth->BindGame({44, 71});
  bool revoke = false;
  int calls = 0;
  sessions::ClanSession session(
      auth, [](auto) { return std::optional{features::clans::ClanSnapshot{}}; },
      features::clans::ClanAuthentication::Clock::now,
      [&](const auto& identity, const auto& request, auto observed,
          const auto& authorized) {
        ++calls;
        Check(identity == features::clans::ClanIdentity{44, 71} &&
              request.name == u"ClanName");
        Check(observed > 1704067200 && authorized());
        if (revoke) {
          game.reset();
          Check(!authorized());
          throw std::runtime_error("Synthetic revoked transaction");
        }
        return protocol::ClanCreateResponse{1, request.item_id, 0};
      });
  Check(!session
             .Process(protocol::ClanLoginRequest{auth->Issue(game, u"Fighter")}
                          .Encode())
             .close_connection);
  const auto packet =
      protocol::ClanCreateRequest{u"ClanName", 0, u"Hi", 100819, 30}.Encode();
  auto result = session.Process(packet);
  Check(!result.close_connection && result.responses.size() == 1);
  Check(protocol::ClanCreateResponse::Decode(result.responses[0]) ==
        protocol::ClanCreateResponse{1, 100819, 0});
  revoke = true;
  Rejects([&] { session.Process(packet); });
  Check(calls == 2 && session.Poll().close_connection);
}
}  // namespace
int RunClanCreationTests(const std::filesystem::path& directory) {
  Fixtures(directory);
  Preparation();
  Session();
  std::cout << "Clan creation: " << checks << " checks passed\n";
  return checks;
}
