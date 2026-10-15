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
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Party session line " + std::to_string(at.line()));
}
template <class Action>
void Rejects(Action action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> ids) {
  Check(packets.size() == ids.size());
  auto expected = ids.begin();
  for (const auto& packet : packets) Check(packet.message_id == *expected++);
}
struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record{17, 7, u"Alice", 27, 2};
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    return id == 7 ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected party character creation");
  }
};
}  // namespace

int RunPartySessionTests() {
  using Session = bf::sessions::PlayerSession;
  using Practice = bf::features::practice::PracticeSession;
  using bf::features::parties::PartyJoinStatus;
  auto now = Practice::Clock::now();
  auto registry = std::make_shared<bf::features::parties::PartyRegistry>(4);
  auto queue = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  auto bob = registry->Enter({18, 8, u"Bob", 10, 7}, queue->Sink());
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
        nullptr, nullptr, registry);
  };
  const auto login = [](Session& session) {
    proto::ClientToServerMessage0x0001 hello;
    hello.initial_value = 1256;
    session.Process(hello.Encode());
    proto::LoginRequest request;
    request.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    const auto packets = session.Process(request.Encode());
    Sequence(packets, {3, 201});
    const auto roster = proto::UpdatePartyNotification::Decode(packets[1]);
    Check(roster.state == 2 && roster.members.size() == 1);
    Check(roster.members[0].identity_value == 17 &&
          roster.members[0].character_name == u"Alice");
  };
  for (const auto& packet :
       {proto::LeavePartyRequest{}.Encode(),
        proto::KickPartyRequest{u"Bob"}.Encode(),
        proto::ChangePartyLeaderRequest{u"Bob"}.Encode()}) {
    auto early = player();
    Rejects([&] { early->Process(packet); });
    Check(early->State() == bf::sessions::PlayerSessionState::kRejected);
    Check(!registry->Capture(u"Alice"));
    auto before_menu = player();
    login(*before_menu);
    Check(bob.JoinApproved(*registry->Capture(u"Alice")) ==
          PartyJoinStatus::kBusy);
    Rejects([&] { before_menu->Process(packet); });
    Check(!registry->Capture(u"Alice"));
  }
  auto session = player();
  login(*session);
  session->Process(proto::ClientToServerMessage0x0209{}.Encode());
  Check(session->Poll().empty());
  const auto capture = *registry->Capture(u"Alice");
  // Internal, pre-authorized membership fixture. This is not a public network
  // admission path, and does not validate invitation consent or scene movement.
  const auto join = [&] {
    Check(bob.JoinApproved(*registry->Capture(u"Alice")) ==
          PartyJoinStatus::kJoined);
    Sequence(queue->Drain(), {201, 206});
    const auto packets = session->Poll();
    Sequence(packets, {201, 206});
    Check(proto::UpdatePartyNotification::Decode(packets[0]).members.size() ==
          2);
    Check(
        proto::PartyMemberJoinedNotification::Decode(packets[1]).member_name ==
        u"Bob");
  };
  join();
  auto packets =
      session->Process(proto::ChangePartyLeaderRequest{u"Bob"}.Encode());
  Sequence(packets, {201, 303});
  Check(proto::UpdatePartyNotification::Decode(packets[0])
            .members[0]
            .character_name == u"Bob");
  Check(proto::ChangePartyLeaderResponse::Decode(packets[1]) ==
        proto::ChangePartyLeaderResponse{u"Bob", u"Alice", 0});
  Sequence(queue->Drain(), {201, 303});
  packets = session->Process(proto::KickPartyRequest{u"Bob"}.Encode());
  Sequence(packets, {302});
  Check(proto::KickPartyResponse::Decode(packets[0]).reason == 4);
  Check(queue->Drain().empty());
  bob.ChangeLeader(u"Alice");
  Sequence(queue->Drain(), {201, 303});
  Sequence(session->Poll(), {201, 303});
  packets = session->Process(proto::KickPartyRequest{u"Bob"}.Encode());
  Sequence(packets, {201, 302});
  Check(proto::KickPartyResponse::Decode(packets[1]).reason == 0);
  Sequence(queue->Drain(), {201, 304});
  Check(bob.JoinApproved(capture) == PartyJoinStatus::kStale);
  join();
  packets = session->Process(proto::LeavePartyRequest{}.Encode());
  Sequence(packets, {201, 204});
  const auto departed = queue->Drain();
  Sequence(departed, {201, 203});
  Check(proto::PartyLeaderLeftNotification::Decode(departed[1]) ==
        proto::PartyLeaderLeftNotification{u"Alice", u"Bob"});
  Check(session->Process(proto::LeavePartyRequest{}.Encode()).empty());
  now += std::chrono::seconds(4);
  // Party administration shares the existing scene-chat five-action window.
  for (int i = 0; i < 4; ++i) {
    packets = session->Process(proto::KickPartyRequest{u"Offline"}.Encode());
    Check(proto::KickPartyResponse::Decode(packets.at(0)).reason == 1);
  }
  Sequence(session->Process(proto::ChatRequest{0, u"hello", {}}.Encode()),
           {509});
  Sequence(session->Process(proto::ChangePartyLeaderRequest{u"Bob"}.Encode()),
           {509});
  Check(queue->Drain().empty());
  now += std::chrono::seconds(4);
  packets = session->Process(proto::ChangePartyLeaderRequest{u"Bob"}.Encode());
  Check(proto::ChangePartyLeaderResponse::Decode(packets.at(0)).reason == 1);
  join();
  Rejects([&] { session->Process({202, {1}}); });
  Check(session->State() == bf::sessions::PlayerSessionState::kRejected);
  Check(!registry->Capture(u"Alice"));
  Sequence(queue->Drain(), {201, 203});
  session.reset();
  session = player();
  login(*session);
  session->Process(proto::ClientToServerMessage0x0209{}.Encode());
  join();
  session.reset();
  Check(!registry->Capture(u"Alice"));
  Sequence(queue->Drain(), {201, 203});
  Check(bob.Snapshot().members.size() == 1);
  std::cout << checks << " party authentication and routing checks passed\n";
  return checks;
}
