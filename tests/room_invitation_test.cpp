#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "features/rooms/room_actions.h"
#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_packet_queue.h"
#include "features/social/room_invitations.h"
#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/rooms/create_room_request.h"
#include "protocol/rooms/leave_room_request.h"
#include "protocol/unresolved/client_to_server_message_0x0209.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace social = bf::features::social;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Room invitation line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
proto::TcpPacket Recode(const proto::TcpPacket& packet) {
  switch (packet.message_id) {
    case 110:
      return proto::InvitationRequest::Decode(packet).Encode();
    case 111:
      return proto::DeclineInvitationRequest::Decode(packet).Encode();
    case 115:
      return proto::MoveToInviterRequest::Decode(packet).Encode();
    case 119:
      return proto::InvitationFailedNotification::Decode(packet).Encode();
    case 120:
      return proto::InvitationDeliveredNotification::Decode(packet).Encode();
    case 121:
      return proto::AskedInvitationNotification::Decode(packet).Encode();
    case 122:
      return proto::MoveToInviterFailedNotification::Decode(packet).Encode();
    default:
      throw std::logic_error("Unexpected invitation fixture");
  }
}
void CodecTests(const std::filesystem::path& directory) {
  std::ifstream file(directory / "room_invitation_packets.txt");
  Check(file.good());
  int rows = 0;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line[0] == '#') continue;
    ++rows;
    auto first = line.find('|'), last = line.rfind('|');
    proto::TcpPacket packet{
        static_cast<std::uint16_t>(std::stoul(line.substr(0, first))), {}};
    for (auto i = last + 1; i < line.size(); i += 2) {
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(line.substr(i, 2), nullptr, 16)));
    }
    Check(Recode(packet).payload == packet.payload &&
          Recode(packet).message_id == packet.message_id);
    if (packet.message_id == 110)
      Check(proto::InvitationRequest::Decode(packet).value_at24 == 1);
    if (packet.message_id == 111)
      Check(proto::DeclineInvitationRequest::Decode(packet).invitation_key ==
            0x12345678);
    if (packet.message_id == 115) {
      const auto value = proto::MoveToInviterRequest::Decode(packet);
      Check(value.address == 0x01020304 && value.port == 0x9abc &&
            value.byte_at32 == 1);
    }
    if (packet.message_id == 121) {
      const auto value = proto::AskedInvitationNotification::Decode(packet);
      Check(value.invitation_key == 0x12345678 && value.address == 0x01020304 &&
            value.port == 0x9abc && value.value_at36 == 0x11223344 &&
            value.value_at40 == 0x55667788);
    }
    for (std::size_t size = 0; size < packet.payload.size(); ++size) {
      auto truncated = packet;
      truncated.payload.resize(size);
      Rejects([&] { Recode(truncated); });
    }
    auto extra = packet;
    extra.payload.push_back(0);
    Rejects([&] { Recode(extra); });
    if (packet.message_id != 111) {
      auto huge = packet;
      std::fill_n(huge.payload.begin(), 4, std::uint8_t{0xff});
      Rejects([&] { Recode(huge); });
    }
  }
  Check(rows == 21);
  Rejects([] { proto::InvitationRequest::Decode({109, {}}); });
  Rejects([] { proto::DeclineInvitationRequest::Decode({110, {}}); });
  Rejects([] { proto::MoveToInviterRequest::Decode({114, {}}); });
  Rejects([] { proto::InvitationFailedNotification::Decode({118, {}}); });
  Rejects([] { proto::InvitationDeliveredNotification::Decode({119, {}}); });
  Rejects([] { proto::AskedInvitationNotification::Decode({120, {}}); });
  Rejects([] { proto::MoveToInviterFailedNotification::Decode({121, {}}); });
  Rejects(
      [] { proto::InvitationRequest{std::u16string(33, u'A'), 0}.Encode(); });
  // Unknown scalar values survive codecs; the admission policy rejects them.
  auto unknown =
      proto::AskedInvitationNotification{u"",    0xffffffff, 0xffffffff,
                                         0xffff, 0xffffffff, 0xffffffff}
          .Encode();
  Check(Recode(unknown).payload == unknown.payload);
  Check(Recode(proto::MoveToInviterRequest{u"", 1, 2, 255}.Encode())
            .payload.back() == 255);
  Check(proto::InvitationRequest::Decode(
            proto::InvitationRequest{std::u16string(32, u'X'), 0xffffffff}
                .Encode())
            .target_name.size() == 32);
  Rejects([] { proto::InvitationRequest::Decode({110, {33, 0, 0, 0}}); });
}
struct Fixture {
  social::InvitationLease::Time now{};
  std::shared_ptr<rooms::RoomRegistry> rooms =
      rooms::RoomRegistry::Create(4, [this] { return now; });
  bool blocked = false;
  std::shared_ptr<social::RoomInvitations> invitations =
      std::make_shared<social::RoomInvitations>(
          rooms, [this](auto, auto) { return !blocked; });
  bf::features::characters::CharacterRecord alice{17, 7, u"Alice", 10, 7},
      bob{18, 8, u"Bob", 10, 7}, carol{19, 9, u"Carol", 10, 7};
  std::shared_ptr<rooms::RoomPacketQueue>
      aq = std::make_shared<rooms::RoomPacketQueue>(),
      bq = std::make_shared<rooms::RoomPacketQueue>(),
      cq = std::make_shared<rooms::RoomPacketQueue>();
  social::InvitationLease a = invitations->Enter(alice, aq->Sink(), false),
                          b = invitations->Enter(bob, bq->Sink(), true),
                          c = invitations->Enter(carol, cq->Sink(), true);
  rooms::RoomLease host;
  rooms::RoomOccupant Occupant(
      const bf::features::characters::CharacterRecord& character) {
    auto data = bf::sessions::CreateMenuSnapshot(
        character, {character.account_id, character.character_id, {}},
        1800000000);
    return {0, character.account_id, character, data.member, data.equipment};
  }
  void Create(std::u16string password = {}, std::uint32_t capacity = 8) {
    auto reservation = rooms->Reserve(alice.account_id, alice.character_id);
    rooms::RoomRecord record;
    record.room_id = reservation.reservation->RoomId();
    record.slot_count = capacity;
    record.password = std::move(password);
    record.settings.text_at8 = u"Invitation";
    record.settings.configuration.short_at4 = 1;
    rooms::RoomMapCatalog::DefaultRandom(record.settings.configuration);
    auto member = Occupant(alice);
    member.member.signed_byte_at4 = 1;
    record.occupants.push_back(std::move(member));
    host = std::move(*reservation.reservation)
               .Commit(std::move(record), aq->Sink(), {});
  }
  std::uint32_t Send(std::u16string name = u"Bob") {
    const auto delivered = a.Invite({name, 1}, now);
    Check(delivered.size() == 1 && delivered[0].message_id == 120);
    auto packets = (name == u"Bob" ? bq : cq)->Drain();
    Check(packets.size() == 1 && packets[0].message_id == 121);
    auto asked = proto::AskedInvitationNotification::Decode(packets[0]);
    Check(asked.inviter_name == u"Alice" && asked.address == 0 &&
          asked.port == 0);
    return asked.invitation_key;
  }
  social::InvitationAdmission Accept(proto::MoveToInviterRequest request = {
                                         u"Alice", 0, 0, 1}) {
    return b.Accept(request, Occupant(bob), bq->Sink(), now);
  }
};
void RegistryTests() {
  {
    Fixture f;
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Bob", 1}, f.now)[0])
              .reason == 20);
    f.Create(u"secret", 2);
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Alice", 1}, f.now)[0])
              .reason == 1);
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Offline", 1}, f.now)[0])
              .reason == 24);
    f.blocked = true;
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Bob", 1}, f.now)[0])
              .reason == 26);
    Check(f.bq->Drain().empty());
    f.blocked = false;
    auto key = f.Send();
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Bob", 1}, f.now)[0])
              .reason == 20);
    f.c.Decline(key,
                f.now);  // Wrong recipient cannot consume another invitation.
    Check(f.aq->Drain().empty());
    f.b.Decline(key, f.now);
    Check(proto::InvitationFailedNotification::Decode(f.aq->Drain().at(0))
              .reason == 26);
    f.b.Decline(key, f.now);
    Check(f.aq->Drain().empty());
    Check(
        proto::MoveToInviterFailedNotification::Decode(f.Accept().responses[0])
            .reason == 21);
    f.Send();
    Check(
        f.rooms->Join(f.host.RoomId(), {}, f.Occupant(f.bob), f.bq->Sink(), {})
            .status == rooms::RoomJoinStatus::kWrongPassword);
    auto joined = f.Accept();
    Check(static_cast<bool>(joined.room) && joined.responses.empty());
    auto snapshot = f.bq->Drain();
    Check(snapshot.size() == 1 && snapshot[0].message_id == 117);
    Check(f.aq->Drain().at(0).message_id == 503);
    Check(f.rooms->Find(joined.room.RoomId())->occupants.size() == 2);
    Check(!f.Accept().room);
  }
  for (int scenario = 0; scenario < 11; ++scenario) {
    Fixture f;
    f.Create();
    f.Send();
    auto request = proto::MoveToInviterRequest{u"Alice", 0, 0, 1};
    switch (scenario) {
      case 0:
        f.now += std::chrono::seconds(30);
        break;
      case 1:
        f.blocked = true;
        break;
      case 2:
        request.address = 1;
        break;
      case 3:
        request.port = 1;
        break;
      case 4:
        request.byte_at32 = 0;
        break;
      case 5:
        f.b.Update(f.bob, false);
        break;
      case 6:
        f.alice.character_name = u"Renamed";
        f.a.Update(f.alice, false);
        break;
      case 7:
        f.a.Release();
        f.a = f.invitations->Enter(f.alice, f.aq->Sink(), false);
        break;
      case 8:
        f.b.Release();
        f.b = f.invitations->Enter(f.bob, f.bq->Sink(), true);
        break;
      case 9:
        f.host.Release();
        f.Create();
        break;
      case 10:
        Check(f.host.ChangeInventory([&] {
          auto renamed = f.alice;
          renamed.character_name = u"Renamed";
          return rooms::RoomInventoryChange{renamed,
                                            f.Occupant(renamed).equipment};
        }));
        break;
    }
    Check(!f.Accept(request).room);
    Check(!f.Accept().room);
    Check(!f.rooms->FindByCharacter(f.bob.character_id));
    if (scenario == 10) {
      // Registry refresh has not yet seen the room's committed new name.
      Check(proto::InvitationFailedNotification::Decode(
                f.a.Invite({u"Bob", 1}, f.now).at(0))
                .reason == 20);
    }
  }
  {
    Fixture f;
    f.Create({}, 2);
    f.Send();
    f.Send(u"Carol");
    social::InvitationAdmission first, second;
    std::jthread one([&] {
      first = f.b.Accept({u"Alice", 0, 0, 1}, f.Occupant(f.bob), f.bq->Sink(),
                         f.now);
    });
    std::jthread two([&] {
      second = f.c.Accept({u"Alice", 0, 0, 1}, f.Occupant(f.carol),
                          f.cq->Sink(), f.now);
    });
    one.join();
    two.join();
    Check(static_cast<bool>(first.room) != static_cast<bool>(second.room));
    const auto& failed = first.room ? second : first;
    Check(proto::MoveToInviterFailedNotification::Decode(failed.responses[0])
              .reason == 15);
    Check(f.rooms->Find(f.host.RoomId())->occupants.size() == 2);
  }
  {
    Fixture f;
    f.Create();
    f.Send();
    auto guest = f.rooms->Join(f.host.RoomId(), {}, f.Occupant(f.carol),
                               f.cq->Sink(), {});
    guest.room.SetMemberState({1, 0, 0});
    const auto& map = rooms::RoomMapCatalog::Maps().front();
    proto::StartGameRequest request{0,
                                    std::u16string(map.directory),
                                    std::u16string(map.name),
                                    std::u16string(map.key),
                                    map.difficulty,
                                    map.rules,
                                    0};
    Check(rooms::RoomActions::Start(f.host, {}, request) ==
          rooms::RoomStartStatus::kStarted);
    Check(
        proto::MoveToInviterFailedNotification::Decode(f.Accept().responses[0])
            .reason == 8);
    f.host.ReturnToWaiting();
    f.Send();
    guest.room.SetMemberState({1, 0, 0});
    Check(rooms::RoomActions::Start(f.host, {}, request) ==
          rooms::RoomStartStatus::kStarted);
    f.host.ReturnToWaiting();
    Check(!f.Accept().room);  // Returning does not revive an invitation from
                              // before start.
    f.Send();
    guest.room.Release();  // A change of membership invalidates outstanding
                           // invitations.
    Check(!f.Accept().room);
  }
}
struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record;
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    return id == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected creation");
  }
};
void SessionTests() {
  using Session = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  auto now = Practice::Clock::now();
  auto registry = rooms::RoomRegistry::Create(8, [&] { return now; });
  auto invitations = std::make_shared<social::RoomInvitations>(registry);
  auto plazas = std::make_shared<bf::features::lobby::PlazaRegistry>();
  const auto player = [&](std::uint64_t id, std::u16string name) {
    auto store = std::make_shared<Characters>();
    store->record = {id, id, std::move(name), 10, 7};
    return std::make_unique<Session>(
        [id](std::string_view) { return std::optional{id}; }, store,
        [](const auto& member, auto observed) {
          return bf::sessions::CreateMenuSnapshot(
              member, {member.account_id, member.character_id, {}}, observed);
        },
        Session::EquipmentChangeHandler{}, Session::ShopCatalogProvider{},
        Session::ShopPurchaseHandler{}, Session::CashPickupHandler{},
        Session::WeaponPresetHandler{}, Session::ItemUseHandler{},
        [&] { return now; }, Practice::CreateRandomSeed, registry, plazas,
        nullptr, invitations);
  };
  auto host = player(7, u"Alice"), guest = player(8, u"Bob");
  auto early = player(9, u"Early");
  Rejects(
      [&] { early->Process(proto::InvitationRequest{u"Bob", 1}.Encode()); });
  const auto login = [](Session& session) {
    proto::ClientToServerMessage0x0001 hello;
    hello.initial_value = 1256;
    session.Process(hello.Encode());
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    session.Process(login.Encode());
    session.Process(proto::ClientToServerMessage0x0209{}.Encode());
    session.Process({106, {}});
  };
  login(*host);
  login(*guest);
  host->Poll();
  proto::CreateRoomRequest create;
  create.settings.text_at8 = u"Invite";
  create.settings.configuration.short_at4 = 1;
  create.password = u"secret";
  create.settings.boolean_at38 = 1;
  const auto created = host->Process(create.Encode());
  const auto id =
      proto::GotoRoomNotification::Decode(created.back()).descriptor.short_at0;
  guest->Poll();
  Check(host->Process(proto::InvitationRequest{u"Bob", 1}.Encode())
            .at(0)
            .message_id == 120);
  const auto asked =
      proto::AskedInvitationNotification::Decode(guest->Poll().at(0));
  Check(asked.invitation_key != 0 && asked.inviter_name == u"Alice");
  const auto moved = guest->Process(
      proto::MoveToInviterRequest{u"Alice", asked.address, asked.port, 1}
          .Encode());
  Check(moved.size() == 1 && moved[0].message_id == 117);
  Check(guest->State() == bf::sessions::PlayerSessionState::kInRoom);
  Check(host->Poll().at(0).message_id == 503);
  guest->Process(proto::RoomReadyRequest{1, 0, 0}.Encode());
  host->Poll();
  const auto& map = rooms::RoomMapCatalog::Maps().front();
  proto::StartGameRequest start{0,
                                std::u16string(map.directory),
                                std::u16string(map.name),
                                std::u16string(map.key),
                                map.difficulty,
                                map.rules,
                                0};
  Check(host->Process(start.Encode()).back().message_id == 510);
  Check(guest->Poll().back().message_id == 510);
  Check(registry->Find(id)->state == rooms::RoomState::kLoading);
  host->Process({624, {1, 0, 0, 0, 0, 0, 0, 0}});
  guest->Poll();
  guest->Process(proto::LeaveRoomRequest{}.Encode());
  host->Poll();
  Check(registry->Find(id)->occupants.size() == 1);
  Check(guest->Process(proto::MoveToInviterRequest{u"Alice", 0, 0, 1}.Encode())
            .back()
            .message_id == 122);
  now += std::chrono::seconds(4);
  for (int i = 0; i < 4; ++i)
    host->Process(proto::InvitationRequest{u"Offline", 1}.Encode());
  host->Process(proto::ChatRequest{0, u"quota", {}}.Encode());
  Check(proto::InvitationFailedNotification::Decode(
            host->Process(proto::InvitationRequest{u"Bob", 1}.Encode()).back())
            .reason == 20);
  Check(guest->Poll().empty());
}
}  // namespace
int RunRoomInvitationTests(const std::filesystem::path& directory) {
  CodecTests(directory);
  RegistryTests();
  SessionTests();
  std::cout << checks << " room invitation checks passed\n";
  return checks;
}
