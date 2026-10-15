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
    throw std::runtime_error("Room visitation line " +
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
    case 112:
      return proto::VisitationRequest::Decode(packet).Encode();
    case 113:
      return proto::DeclineVisitationRequest::Decode(packet).Encode();
    case 114:
      return proto::AcceptVisitationRequest::Decode(packet).Encode();
    case 115:
      return proto::MoveToInviterRequest::Decode(packet).Encode();
    case 123:
      return proto::VisitationFailedNotification::Decode(packet).Encode();
    case 124:
      return proto::AskedVisitationNotification::Decode(packet).Encode();
    case 125:
      return proto::VisitationAcceptedNotification::Decode(packet).Encode();
    default:
      throw std::logic_error("Unexpected visitation fixture");
  }
}
void CodecTests(const std::filesystem::path& directory) {
  std::ifstream file(directory / "room_visitation_packets.txt");
  Check(file.good());
  int rows = 0;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line[0] == '#') continue;
    ++rows;
    auto first = line.find('|'), last = line.rfind('|');
    proto::TcpPacket packet{
        static_cast<std::uint16_t>(std::stoul(line.substr(0, first))), {}};
    for (auto i = last + 1; i < line.size(); i += 2)
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(line.substr(i, 2), nullptr, 16)));
    Check(Recode(packet).payload == packet.payload &&
          Recode(packet).message_id == packet.message_id);
    const auto expected_name = rows <= 7    ? u"Bob"
                               : rows <= 14 ? u"角色"
                                            : u"LongPlayer12";
    const auto flag = rows <= 7 ? 0U : rows <= 14 ? 1U : 255U;
    if (packet.message_id == 112)
      Check(proto::VisitationRequest::Decode(packet).target_name ==
            expected_name);
    if (packet.message_id == 113) {
      auto value = proto::DeclineVisitationRequest::Decode(packet);
      Check(value.visitation_key == 0x12345678 && value.value_at4 == flag);
    }
    if (packet.message_id == 114)
      Check(proto::AcceptVisitationRequest::Decode(packet).visitation_key ==
            0x12345678);
    if (packet.message_id == 115) {
      auto value = proto::MoveToInviterRequest::Decode(packet);
      Check(value.inviter_name == expected_name &&
            value.address == 0x01020304 && value.port == 0x9abc &&
            value.byte_at32 == 0);
    }
    if (packet.message_id == 123) {
      auto value = proto::VisitationFailedNotification::Decode(packet);
      Check(value.target_name == expected_name && value.reason == 28);
    }
    if (packet.message_id == 124) {
      auto value = proto::AskedVisitationNotification::Decode(packet);
      Check(value.visitor_name == expected_name &&
            value.visitation_key == 0x12345678 && value.value_at28 == flag);
    }
    if (packet.message_id == 125) {
      auto value = proto::VisitationAcceptedNotification::Decode(packet);
      Check(value.target_name == expected_name && value.address == 0x01020304 &&
            value.port == 0x9abc);
    }
    for (std::size_t n = 0; n < packet.payload.size(); ++n) {
      auto truncated = packet;
      truncated.payload.resize(n);
      Rejects([&] { Recode(truncated); });
    }
    auto extra = packet;
    extra.payload.push_back(0);
    Rejects([&] { Recode(extra); });
  }
  Check(rows == 21);
  Rejects([] { proto::VisitationRequest::Decode({111, {}}); });
  Rejects([] { proto::DeclineVisitationRequest::Decode({112, {}}); });
  Rejects([] { proto::AcceptVisitationRequest::Decode({113, {}}); });
  Rejects([] { proto::VisitationFailedNotification::Decode({122, {}}); });
  Rejects([] { proto::AskedVisitationNotification::Decode({123, {}}); });
  Rejects([] { proto::VisitationAcceptedNotification::Decode({124, {}}); });
  Rejects([] { proto::VisitationRequest::Decode({112, {33, 0, 0, 0}}); });
  Rejects([] { proto::VisitationRequest{std::u16string(33, u'X')}.Encode(); });
  Check(proto::VisitationRequest::Decode(
            proto::VisitationRequest{std::u16string(32, u'X')}.Encode())
            .target_name.size() == 32);
  auto unknown =
      proto::AskedVisitationNotification{0xffffffff, u"", 0xffffffff}.Encode();
  Check(Recode(unknown).payload == unknown.payload);
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

std::uint32_t Ask(Fixture& f) {
  Check(f.b.RequestVisit({u"Alice"}, f.now).empty());
  const auto packets = f.aq->Drain();
  Check(packets.size() == 1 && packets[0].message_id == 124);
  const auto asked = proto::AskedVisitationNotification::Decode(packets[0]);
  Check(asked.visitor_name == u"Bob" && asked.value_at28 == 0 &&
        asked.visitation_key != 0);
  return asked.visitation_key;
}
void Approve(Fixture& f, std::uint32_t key) {
  f.a.ApproveVisit(key, f.now);
  const auto packets = f.bq->Drain();
  Check(packets.size() == 1 && packets[0].message_id == 125);
  const auto accepted =
      proto::VisitationAcceptedNotification::Decode(packets[0]);
  Check(accepted.target_name == u"Alice" && accepted.address == 0 &&
        accepted.port == 0);
}
auto Move(Fixture& f) { return f.Accept({u"Alice", 0, 0, 0}); }
auto Reason(const std::vector<proto::TcpPacket>& packets) {
  return proto::VisitationFailedNotification::Decode(packets.at(0)).reason;
}
proto::StartGameRequest Start() {
  const auto& map = rooms::RoomMapCatalog::Maps().front();
  return {0,
          std::u16string(map.directory),
          std::u16string(map.name),
          std::u16string(map.key),
          map.difficulty,
          map.rules,
          0};
}
void RegistryTests() {
  {
    Fixture f;
    Check(Reason(f.b.RequestVisit({u"Alice"}, f.now)) == 20);
    f.Create(u"secret");
    Check(Reason(f.b.RequestVisit({u"Bob"}, f.now)) == 1);
    Check(Reason(f.b.RequestVisit({u"Offline"}, f.now)) == 21);
    f.b.Update(f.bob, false);
    Check(Reason(f.b.RequestVisit({u"Alice"}, f.now)) == 3);
    f.b.Update(f.bob, true);
    f.blocked = true;
    Check(Reason(f.b.RequestVisit({u"Alice"}, f.now)) == 28);
    f.blocked = false;
    auto key = Ask(f);
    Check(Reason(f.b.RequestVisit({u"Alice"}, f.now)) == 20);
    Check(proto::InvitationFailedNotification::Decode(
              f.a.Invite({u"Bob", 1}, f.now).at(0))
              .reason == 20);
    Check(!Move(f).room);  // C115 cannot self-approve an outstanding request.
    f.b.ApproveVisit(key, f.now);
    f.c.ApproveVisit(key, f.now);
    f.c.DeclineVisit({key, 0}, f.now);
    f.b.Decline(key, f.now);
    Check(f.bq->Drain().empty());
    f.a.DeclineVisit(
        {key, 255},
        f.now);  // Unobserved decline flag preserves pending authority.
    Check(f.bq->Drain().empty());
    f.a.DeclineVisit({key, 1}, f.now);
    Check(Reason(f.bq->Drain()) == 28);
    f.a.ApproveVisit(key, f.now);
    Check(f.bq->Drain().empty());
    Check(!Move(f).room);
    key = Ask(f);
    Approve(f, key);
    f.a.ApproveVisit(key, f.now);
    f.a.DeclineVisit({key, 0}, f.now);
    Check(f.bq->Drain().empty());
    Check(
        !f.Accept()
             .room);  // Invitation variant 1 cannot consume a visit as a join.
    Check(!Move(f).room);
    key = Ask(f);
    Approve(f, key);
    auto moved = Move(f);
    Check(static_cast<bool>(moved.room) && moved.responses.empty());
    Check(f.bq->Drain().at(0).message_id == 117);
    Check(f.aq->Drain().at(0).message_id == 503);
    Check(f.rooms->Find(moved.room.RoomId())->password == u"secret");
    Check(!Move(f).room);
  }
  for (int scenario = 0; scenario < 8; ++scenario) {
    Fixture f;
    f.Create();
    auto key = Ask(f);
    switch (scenario) {
      case 0:
        f.now += std::chrono::seconds(30);
        break;
      case 1:
        f.blocked = true;
        break;
      case 2:
        f.host.Release();
        f.Create();
        break;
      case 3:
        f.b.Release();
        f.b = f.invitations->Enter(f.bob, f.bq->Sink(), true);
        break;
      case 4:
        f.a.Release();
        f.a = f.invitations->Enter(f.alice, f.aq->Sink(), false);
        break;
      case 5:
        f.alice.character_name = u"Renamed";
        f.a.Update(f.alice, false);
        break;
      case 6:
        f.b.Update(f.bob, false);
        f.b.Update(f.bob, true);
        break;
      case 7:
        f.bob.character_name = u"Changed";
        f.b.Update(f.bob, true);
        break;
    }
    f.a.ApproveVisit(key, f.now);
    for (auto& packet : f.bq->Drain()) Check(packet.message_id == 123);
    Check(!Move(f).room);
    Check(!f.rooms->FindByCharacter(f.bob.character_id));
  }
  {
    Fixture f;
    f.Create();
    auto key = Ask(f);
    f.now += std::chrono::seconds(29);
    Approve(f, key);
    f.now += std::chrono::seconds(1);
    Check(!Move(f).room);
  }
  for (bool before_approval : {true, false}) {
    Fixture f;
    f.Create({}, 2);
    auto key = Ask(f);
    if (!before_approval) Approve(f, key);
    auto other = f.rooms->Join(f.host.RoomId(), {}, f.Occupant(f.carol),
                               f.cq->Sink(), {});
    if (before_approval) {
      f.a.ApproveVisit(key, f.now);
      Check(Reason(f.bq->Drain()) == 15);
    } else
      Check(proto::MoveToInviterFailedNotification::Decode(
                Move(f).responses.at(0))
                .reason == 15);
  }
  for (bool before_approval : {true, false}) {
    Fixture f;
    f.Create();
    auto other = f.rooms->Join(f.host.RoomId(), {}, f.Occupant(f.carol),
                               f.cq->Sink(), {});
    f.aq->Clear();
    auto key = Ask(f);
    if (!before_approval) Approve(f, key);
    other.room.SetMemberState({1, 0, 0});
    Check(rooms::RoomActions::Start(f.host, {}, Start()) ==
          rooms::RoomStartStatus::kStarted);
    if (before_approval) {
      f.a.ApproveVisit(key, f.now);
      Check(Reason(f.bq->Drain()) == 8);
    } else
      Check(proto::MoveToInviterFailedNotification::Decode(
                Move(f).responses.at(0))
                .reason == 8);
    f.host.ReturnToWaiting();
    Check(!Move(f).room);
  }
  {
    Fixture f;
    f.Create({}, 2);
    auto key = Ask(f);
    Approve(f, key);
    f.Send(u"Carol");
    social::InvitationAdmission visit, invite;
    std::jthread first([&] { visit = Move(f); });
    std::jthread second([&] {
      invite = f.c.Accept({u"Alice", 0, 0, 1}, f.Occupant(f.carol),
                          f.cq->Sink(), f.now);
    });
    first.join();
    second.join();
    Check(static_cast<bool>(visit.room) != static_cast<bool>(invite.room));
    Check(f.rooms->Find(f.host.RoomId())->occupants.size() == 2);
  }
  {
    Fixture f;
    f.Create();
    std::vector<social::InvitationLease> applicants;
    std::vector<std::uint32_t> keys;
    for (std::uint64_t n = 0; n < 17; ++n) {
      bf::features::characters::CharacterRecord character{
          100 + n, 100 + n, std::u16string(1, static_cast<char16_t>(u'A' + n)),
          10, 7};
      applicants.push_back(f.invitations->Enter(character, f.bq->Sink(), true));
      const auto packets = applicants.back().RequestVisit({u"Alice"}, f.now);
      if (n < 16) {
        Check(packets.empty());
        keys.push_back(
            proto::AskedVisitationNotification::Decode(f.aq->Drain().at(0))
                .visitation_key);
      } else
        Check(Reason(packets) == 20);
    }
    Check(f.aq->Drain().empty());
    f.a.DeclineVisit({keys.front(), 0}, f.now);
    Check(Reason(f.bq->Drain()) == 28);
    Check(applicants.back().RequestVisit({u"Alice"}, f.now).empty());
    Check(f.aq->Drain().size() == 1);
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
  auto early = player(9, u"Early");
  for (const auto& packet : {proto::VisitationRequest{u"Alice"}.Encode(),
                             proto::DeclineVisitationRequest{7, 0}.Encode(),
                             proto::AcceptVisitationRequest{7}.Encode()})
    Rejects([&] { early->Process(packet); });
  auto host = player(7, u"Alice"), guest = player(8, u"Bob");
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
  create.settings.text_at8 = u"Visit";
  create.settings.configuration.short_at4 = 1;
  host->Process(create.Encode());
  guest->Poll();
  for (int i = 0; i < 5; ++i)
    guest->Process(proto::VisitationRequest{u"Offline"}.Encode());
  Check(Reason(guest->Process(proto::VisitationRequest{u"Alice"}.Encode())) ==
        20);
  Check(host->Poll().empty());
  now += std::chrono::seconds(4);
  Check(guest->Process(proto::VisitationRequest{u"Alice"}.Encode()).empty());
  const auto asked =
      proto::AskedVisitationNotification::Decode(host->Poll().at(0));
  for (int i = 0; i < 5; ++i)
    host->Process(proto::InvitationRequest{u"Offline", 1}.Encode());
  host->Process(proto::AcceptVisitationRequest{asked.visitation_key}.Encode());
  Check(guest->Poll().empty());
  Check(guest->Process(proto::MoveToInviterRequest{u"Alice", 0, 0, 0}.Encode())
            .at(0)
            .message_id == 122);
  now += std::chrono::seconds(4);
  host->Process(proto::AcceptVisitationRequest{asked.visitation_key}.Encode());
  Check(guest->Poll().at(0).message_id == 125);
  const auto moved =
      guest->Process(proto::MoveToInviterRequest{u"Alice", 0, 0, 0}.Encode());
  Check(moved.size() == 1 && moved[0].message_id == 117);
  Check(guest->State() == bf::sessions::PlayerSessionState::kInRoom);
  Check(host->Poll().at(0).message_id == 503);
  guest->Process(proto::LeaveRoomRequest{}.Encode());
  host->Poll();
  now += std::chrono::seconds(4);
  guest->Process(proto::VisitationRequest{u"Alice"}.Encode());
  const auto next =
      proto::AskedVisitationNotification::Decode(host->Poll().at(0));
  for (int i = 0; i < 5; ++i)
    host->Process(proto::InvitationRequest{u"Offline", 1}.Encode());
  host->Process(
      proto::DeclineVisitationRequest{next.visitation_key, 0}.Encode());
  Check(Reason(guest->Poll()) ==
        28);  // Refusal remains possible after quota exhaustion.
}
}  // namespace
int RunRoomVisitationTests(const std::filesystem::path& directory) {
  CodecTests(directory);
  RegistryTests();
  SessionTests();
  std::cout << checks << " room visitation checks passed\n";
  return checks;
}
