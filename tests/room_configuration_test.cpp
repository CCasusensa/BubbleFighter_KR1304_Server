#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_creation.h"
#include "features/rooms/room_map_catalog.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/room_configuration_request.h"
#include "protocol/rooms/room_slot_update_notification.h"
#include "protocol/scenes/scene_descriptor_notification.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Room configuration check at " +
                             std::to_string(at.line()));
}
template <typename Action>
void Rejects(Action action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
proto::RoomConfigurationRequest Select(const rooms::RoomMapDefinition& map) {
  proto::RoomConfigurationRequest request;
  request.map_name = map.name;
  request.map_directory = map.directory;
  request.map_key = map.key;
  request.values_at76[6] = map.difficulty;
  request.values_at76[7] = 1;
  request.bytes_at112[1] = 1;
  return request;
}
proto::StartGameRequest Start(const rooms::RoomMapDefinition& map) {
  return {0,
          std::u16string(map.directory),
          std::u16string(map.name),
          std::u16string(map.key),
          map.difficulty,
          map.rules,
          0};
}
void CodecTests(const std::filesystem::path& fixture) {
  std::ifstream file(fixture);
  Check(file.good());
  int rows = 0;
  for (std::string name, hex; file >> name >> hex;) {
    proto::TcpPacket packet{513, {}};
    for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(hex.substr(offset, 2), nullptr, 16)));
    }
    const auto request = proto::RoomConfigurationRequest::Decode(packet);
    Check(request.Encode().payload == packet.payload);
    if (name == "Distinct") {
      Check(request.map_name == u"地图名称12345678" &&
            request.map_directory == u"DirectoryLong");
      Check(request.map_key == u"Key" && request.bytes_at72[0] == 255 &&
            request.values_at76[6] == 107 && request.bytes_at112[1] == 252);
    }
    for (std::size_t length = 0; length < packet.payload.size(); ++length) {
      auto truncated = packet;
      truncated.payload.resize(length);
      Rejects([&] { proto::RoomConfigurationRequest::Decode(truncated); });
    }
    auto wrong = packet;
    wrong.message_id = 514;
    Rejects([&] { proto::RoomConfigurationRequest::Decode(wrong); });
    wrong = packet;
    wrong.payload.push_back(0);
    Rejects([&] { proto::RoomConfigurationRequest::Decode(wrong); });
    // A malicious first string count must be checked before allocation/copy.
    wrong = packet;
    for (std::size_t i = 0; i < 4; ++i) wrong.payload[i] = 0xff;
    Rejects([&] { proto::RoomConfigurationRequest::Decode(wrong); });
    ++rows;
  }
  Check(rows == 3);
  // The four byte-origin fields are DWORDs on the wire, but cannot exceed 255.
  for (const std::size_t offset : {12U, 16U, 56U, 60U}) {
    proto::TcpPacket invalid{513, std::vector<std::uint8_t>(64)};
    invalid.payload[offset + 1] = 1;
    Rejects([&] { proto::RoomConfigurationRequest::Decode(invalid); });
  }
  proto::RoomConfigurationRequest invalid;
  invalid.bytes_at112[1] = 256;
  Rejects([&] { invalid.Encode(); });
}
void RegistryTests() {
  using bf::features::characters::CharacterRecord;
  const CharacterRecord host{17, 77, u"MapHost", 10, 7};
  const CharacterRecord guest{18, 78, u"MapGuest", 10, 7};
  const CharacterRecord late{19, 79, u"MapLate", 10, 7};
  const auto host_data =
      bf::sessions::CreateMenuSnapshot(host, {77, 17, {}}, 1);
  const auto guest_data =
      bf::sessions::CreateMenuSnapshot(guest, {78, 18, {}}, 1);
  const auto late_data =
      bf::sessions::CreateMenuSnapshot(late, {79, 19, {}}, 1);
  auto registry = rooms::RoomRegistry::Create(8);
  std::vector<proto::TcpPacket> host_packets, guest_packets, late_packets;
  const auto sink = [](auto& target) {
    return [&target](const auto& packets) {
      target.insert(target.end(), packets.begin(), packets.end());
    };
  };
  proto::CreateRoomRequest request;
  request.settings = host_data.scene.descriptor;
  request.settings.short_at0 = 0;
  request.settings.session_type = 0;
  request.settings.text_at8 = u"MapChanges";
  request.settings.configuration.texts_at44[0] = u"Byway";
  auto created = rooms::RoomCreation::Create(
      *registry, {{1}, {0}, 20, 6}, request,
      {77, host, host_data.member, host_data.equipment, sink(host_packets)});
  Check(static_cast<bool>(created.room));
  const auto id = created.room.RoomId();
  Check(proto::GotoRoomNotification::Decode(host_packets.back())
            .descriptor.configuration.texts_at44[0] == u"Random");
  auto joined =
      rooms::RoomActions::Join(*registry, {id, {}},
                               {78, guest, guest_data.member,
                                guest_data.equipment, sink(guest_packets)});
  Check(static_cast<bool>(joined.room));
  Check(rooms::RoomMapCatalog::Maps().size() == 78);
  const auto& map = rooms::RoomMapCatalog::Maps()[1];
  const auto selection = Select(map);
  const auto unchanged = *registry->Find(id);
  host_packets.clear();
  guest_packets.clear();
  Check(!joined.room.Configure(selection));
  Check(*registry->Find(id) == unchanged && host_packets.empty() &&
        guest_packets.empty());
  for (const auto& entry : rooms::RoomMapCatalog::Maps()) {
    Check(joined.room.SetMemberState({1, 0, 0}));
    host_packets.clear();
    guest_packets.clear();
    auto choose = Select(entry);
    choose.map_name = u"Untrusted display text";
    Check(created.room.Configure(choose));
    Check(host_packets.size() == 2 && guest_packets.size() == 2);
    Check(proto::TcpPacketCodec::Encode(host_packets[0]) ==
          proto::TcpPacketCodec::Encode(guest_packets[0]));
    Check(proto::TcpPacketCodec::Encode(host_packets[1]) ==
          proto::TcpPacketCodec::Encode(guest_packets[1]));
    Check(host_packets[0].message_id == 501 &&
          host_packets[1].message_id == 503);
    const auto descriptor =
        proto::SceneDescriptorNotification::Decode(host_packets[0]).descriptor;
    Check(descriptor.configuration.texts_at44[0] == entry.key &&
          descriptor.configuration.texts_at44[1] == entry.directory &&
          descriptor.configuration.texts_at44[2] == entry.name &&
          descriptor.configuration.value_at140 == entry.difficulty);
    const auto member =
        proto::RoomSlotUpdateNotification::Decode(host_packets[1]);
    Check(member.slot == 1 && member.member.booleans_at96[0] == 0);
    Check(registry->Find(id)->FindSlot(1)->member.booleans_at96[0] == 0);
    Check(registry->List(0, 8).rooms[0].configuration ==
          descriptor.configuration);
  }
  Check(created.room.Configure(selection));
  const auto before = *registry->Find(id);
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto invalid = selection;
    if (mutation == 0) invalid.map_key = u"../../Unknown";
    if (mutation == 1) ++invalid.values_at76[6];
    if (mutation == 2) ++invalid.values_at76[7];
    if (mutation == 3) ++invalid.values_at76[8];
    if (mutation == 4) invalid.map_directory = u"UntrustedDirectory";
    host_packets.clear();
    guest_packets.clear();
    Check(!created.room.Configure(invalid));
    Check(*registry->Find(id) == before && host_packets.empty() &&
          guest_packets.empty());
  }
  auto later = rooms::RoomActions::Join(
      *registry, {id, {}},
      {79, late, late_data.member, late_data.equipment, sink(late_packets)});
  Check(static_cast<bool>(later.room));
  Check(proto::GotoRoomNotification::Decode(late_packets.back())
            .descriptor.configuration == before.settings.configuration);
  Check(later.room.SetMemberState({1, 0, 0}));
  Check(joined.room.SetMemberState({1, 0, 0}));
  Check(rooms::RoomActions::Start(created.room, {},
                                  Start(rooms::RoomMapCatalog::Maps()[0])) ==
        rooms::RoomStartStatus::kUnsupportedMap);
  // Calling the internal API without a trusted key must not bypass a fixed map.
  Check(created.room.Start({510, {}}, 1, 1000) ==
        rooms::RoomStartStatus::kUnsupportedMap);
  Check(created.room.ChangeInventory([&] {
    Check(!created.room.Configure(Select(rooms::RoomMapCatalog::Maps()[0])));
    Check(rooms::RoomActions::Start(created.room, {}, Start(map)) ==
          rooms::RoomStartStatus::kMembersNotReady);
    return rooms::RoomInventoryChange{host, host_data.equipment};
  }));
  Check(rooms::RoomActions::Start(created.room, {}, Start(map)) ==
        rooms::RoomStartStatus::kStarted);
  Check(!created.room.Configure(selection));
  Check(proto::GameLoadNotification::Decode(host_packets.back()).texts_at0[2] ==
        map.key);
  created.room.ReturnToWaiting();
  Check(registry->Find(id)->settings.configuration.texts_at44[0] == map.key);

  proto::RoomConfigurationRequest random;
  random.map_name = random.map_directory = random.map_key = u"Random";
  random.bytes_at72 = {1, 1};
  // Original UI observation: previous difficulty survives changing to Random.
  random.values_at76 = {240000, 20, 0, 0, 0, 0, 5, 1, 0};
  Check(created.room.Configure(random));
  const auto normalized = registry->Find(id)->settings.configuration;
  Check(normalized.texts_at44 ==
        std::array<std::u16string, 4>{u"Random", u"", u"", u""});
  Check(normalized.texts_at144 ==
        std::array<std::u16string, 6>{u"Random", u"", u"Random", u"", u"Random",
                                      u""});
  Check(normalized.value_at140 == 1 && normalized.short_at4 == 1);
  random.map_directory = map.directory;
  Check(!created.room.Configure(random));
  Check(registry->Find(id)->settings.configuration == normalized);
  Check(joined.room.SetMemberState({1, 0, 0}));
  Check(later.room.SetMemberState({1, 0, 0}));
  auto start = Start(map);
  start.map_key = u"Random";
  Check(rooms::RoomActions::Start(created.room, {}, start) ==
        rooms::RoomStartStatus::kStarted);
  Check(proto::GameLoadNotification::Decode(host_packets.back()).texts_at0[2] ==
        map.key);
  created.room.ReturnToWaiting();
  Check(registry->Find(id)->settings.configuration == normalized);
  created.room.Release();
  Check(registry->Find(id)->host_slot == 1);
  Check(joined.room.Configure(selection));
  rooms::RoomLease released;
  Check(!released.Configure(selection));
}

struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord character{17, 77, u"MapHost", 10,
                                                      7};
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t) override {
    return character;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    return bf::features::characters::CharacterCreateResult::
        kAccountHasCharacter;
  }
};
void RoutingTests() {
  auto registry = rooms::RoomRegistry::Create(8);
  const auto make_player = [&](std::uint64_t account, std::uint64_t character,
                               std::u16string name) {
    auto characters = std::make_shared<Characters>();
    characters->character = {character, account, std::move(name), 10, 7};
    return std::make_unique<bf::sessions::PlayerSession>(
        [account](std::string_view) { return std::optional{account}; },
        characters,
        [](const auto& member, auto now) {
          return bf::sessions::CreateMenuSnapshot(
              member, {member.account_id, member.character_id, {}}, now);
        },
        bf::sessions::PlayerSession::EquipmentChangeHandler{},
        bf::sessions::PlayerSession::ShopCatalogProvider{},
        bf::sessions::PlayerSession::ShopPurchaseHandler{},
        bf::sessions::PlayerSession::CashPickupHandler{},
        bf::sessions::PlayerSession::WeaponPresetHandler{},
        bf::sessions::PlayerSession::ItemUseHandler{},
        bf::features::practice::PracticeSession::Clock::now,
        bf::features::practice::PracticeSession::CreateRandomSeed, registry);
  };
  auto host = make_player(77, 17, u"MapHost");
  auto guest = make_player(78, 18, u"MapGuest");
  const auto login = [](auto& player) {
    proto::ClientToServerMessage0x0001 first;
    first.initial_value = 1256;
    Check(player.Process(first.Encode()).size() == 2);
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    Check(player.Process(login.Encode()).size() == 2);
    Check(
        player.Process(proto::ClientToServerMessage0x0209{}.Encode()).size() ==
        5);
    Check(!player.Process({106, {}}).empty());
  };
  login(*host);
  login(*guest);
  proto::CreateRoomRequest create;
  create.settings.text_at8 = u"Routing";
  create.settings.configuration.short_at4 = 1;
  auto packets = host->Process(create.Encode());
  Check(packets.size() == 2 && packets[0].message_id == 102 &&
        packets[1].message_id == 117);
  const auto id =
      proto::GotoRoomNotification::Decode(packets[1]).descriptor.short_at0;
  Check(guest->Process(proto::JoinRoomRequest{id, {}}.Encode()).size() == 2);
  host->Poll();
  const auto request = Select(rooms::RoomMapCatalog::Maps()[1]).Encode();
  packets = guest->Process(request);
  Check(packets.size() == 1 && packets[0].message_id == 509);
  Check(guest->State() == bf::sessions::PlayerSessionState::kInRoom);
  Check(host->Poll().empty());
  Check(host->Process(request).front().message_id == 501);
  Check(guest->Poll().front().message_id == 501);
  auto invalid = proto::RoomConfigurationRequest::Decode(request);
  ++invalid.values_at76[7];
  Check(host->Process(invalid.Encode()).front().message_id == 509);
  Check(host->State() == bf::sessions::PlayerSessionState::kInRoom);
  Check(guest->Poll().empty());
  bf::sessions::PlayerSession unauthenticated;
  Rejects([&] { unauthenticated.Process(request); });
  Check(unauthenticated.State() == bf::sessions::PlayerSessionState::kRejected);
}
}  // namespace

int RunRoomConfigurationTests(const std::filesystem::path& fixture) {
  CodecTests(fixture);
  RegistryTests();
  RoutingTests();
  std::cout << checks << " room configuration checks passed\n";
  return checks;
}
