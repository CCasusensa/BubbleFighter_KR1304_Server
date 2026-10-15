#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "features/social/room_invitations.h"
#include "sessions/menu_snapshot.h"

namespace bf = bubble_fighter::server;
namespace social = bf::features::social;
namespace parties = bf::features::parties;
namespace lobby = bf::features::lobby;
namespace rooms = bf::features::rooms;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Party consent line " + std::to_string(at.line()));
}
template <class Action>
void Rejects(Action action) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
void Sequence(const std::vector<proto::TcpPacket>& packets,
              std::initializer_list<std::uint16_t> ids) {
  Check(packets.size() == ids.size());
  auto it = ids.begin();
  for (const auto& packet : packets) Check(packet.message_id == *it++);
}
struct Fixture {
  social::InvitationLease::Time now{};
  std::optional<std::pair<std::uint64_t, std::uint64_t>> blocked;
  std::shared_ptr<rooms::RoomRegistry> rooms = rooms::RoomRegistry::Create(4);
  std::shared_ptr<lobby::PlazaRegistry> plazas =
      std::make_shared<lobby::PlazaRegistry>();
  std::shared_ptr<parties::PartyRegistry> parties;
  std::shared_ptr<social::RoomInvitations> invitations;
  std::array<std::shared_ptr<rooms::RoomPacketQueue>, 18> queues, scenes;
  std::array<parties::PartyMembership, 18> members;
  std::array<social::InvitationLease, 18> leases;
  std::array<lobby::PlazaLease, 18> locations;
  explicit Fixture(std::size_t capacity = 8) {
    const auto allows = [this](auto a, auto b) {
      return !blocked ||
             (*blocked != std::pair{a, b} && *blocked != std::pair{b, a});
    };
    parties = std::make_shared<parties::PartyRegistry>(capacity, allows);
    invitations = std::make_shared<social::RoomInvitations>(rooms, allows,
                                                            parties, plazas);
    for (std::size_t i = 0; i < members.size(); ++i) {
      queues[i] = std::make_shared<rooms::RoomPacketQueue>();
      scenes[i] = std::make_shared<rooms::RoomPacketQueue>();
      members[i] = parties->Enter(Character(i), queues[i]->Sink());
      leases[i] = invitations->Enter(Character(i), queues[i]->Sink(), true);
    }
    Clear();
  }
  static bf::features::characters::CharacterRecord Character(std::size_t i) {
    return {100 + i, 200 + i,
            u"Peer" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 10,
            7};
  }
  static bf::sessions::MenuInitializationData Data(std::size_t i) {
    const auto c = Character(i);
    return bf::sessions::CreateMenuSnapshot(
        c, {c.account_id, c.character_id, {}}, 1000);
  }
  static rooms::RoomOccupant Occupant(std::size_t i) {
    auto d = Data(i);
    return {0, Character(i).account_id, Character(i), std::move(d.member),
            std::move(d.equipment)};
  }
  social::PartyAdmissionInput Input(std::size_t i) {
    return {&members[i], Data(i).scene, scenes[i]->Sink(),
            locations[i].Token()};
  }
  social::InvitationAdmission Accept(std::size_t visitor, std::size_t owner = 0,
                                     std::uint8_t kind = 1,
                                     std::uint32_t address = 0) {
    auto input = Input(visitor);
    auto result = leases[visitor].Accept(
        {Character(owner).character_name, address, 0, kind}, Occupant(visitor),
        scenes[visitor]->Sink(), now, &input);
    if (result.plaza || result.moved_to_menu) {
      // The session must adopt/release its scene before it can be a target.
      const auto early =
          leases[visitor].Invite({Character(17).character_name, 1}, now);
      if (early.size() != 1 || early[0].message_id != 119 ||
          proto::InvitationFailedNotification::Decode(early[0]).reason != 20)
        throw std::runtime_error("Acceptance exposed an unadopted scene");
      leases[visitor].Update(Character(visitor), true);
    }
    return result;
  }
  std::uint32_t Invite(std::size_t owner, std::size_t visitor,
                       std::uint32_t flag = 1) {
    Sequence(
        leases[owner].Invite({Character(visitor).character_name, flag}, now),
        {120});
    auto packets = queues[visitor]->Drain();
    Sequence(packets, {121});
    auto asked = proto::AskedInvitationNotification::Decode(packets[0]);
    Check(asked.inviter_name == Character(owner).character_name &&
          asked.address == 0 && asked.port == 0);
    return asked.invitation_key;
  }
  void Failure(social::InvitationAdmission result, std::uint32_t reason) {
    Check(!result.room && !result.plaza && !result.moved_to_menu);
    Sequence(result.responses, {122});
    Check(proto::MoveToInviterFailedNotification::Decode(result.responses[0])
              .reason == reason);
  }
  void Roster(std::size_t i, std::initializer_list<std::size_t> expected) {
    const auto roster = members[i].Snapshot();
    Check(roster.state == 2 && roster.members.size() == expected.size());
    auto at = roster.members.begin();
    for (auto id : expected) {
      Check(at->identity_value == Character(id).character_id &&
            at->character_name == Character(id).character_name);
      ++at;
    }
  }
  void Joined(std::size_t i, std::initializer_list<std::size_t> expected) {
    Roster(i, expected);
    const auto packets = queues[i]->Drain();
    Sequence(packets, {201, 206});
    Check(proto::UpdatePartyNotification::Decode(packets[0]).members ==
          members[i].Snapshot().members);
  }
  void Scene(std::size_t i, std::uint32_t type, std::size_t count) {
    auto packets = scenes[i]->Drain();
    Sequence(packets, {117});
    auto scene = proto::GotoRoomNotification::Decode(packets[0]);
    Check(scene.descriptor.session_type == type &&
          scene.records_at564.size() == count);
    Check(scene.members.at(scene.value_at728).value_at8 ==
          Character(i).character_id);
  }
  void Clear() {
    for (auto& q : queues) q->Clear();
    for (auto& q : scenes) q->Clear();
  }
  void Empty() {
    for (auto& q : queues) Check(q->Drain().empty());
    for (auto& q : scenes) Check(q->Drain().empty());
  }
  void Plaza(std::size_t i) {
    auto d = Data(i);
    locations[i] = plazas->Enter(d.scene, {d.member, Character(i), d.equipment},
                                 Character(i).account_id, scenes[i]->Sink());
  }
};
void Consent() {
  Fixture f;
  f.Failure(f.Accept(1), 21);
  auto key = f.Invite(0, 1, 0);
  f.leases[2].Decline(key, f.now);
  f.Empty();
  f.leases[1].Decline(key, f.now);
  auto declined = f.queues[0]->Drain();
  Sequence(declined, {119});
  Check(proto::InvitationFailedNotification::Decode(declined[0]).reason == 26);
  f.Failure(f.Accept(1), 21);
  f.Invite(0, 1);
  f.now += std::chrono::seconds(30);
  f.Failure(f.Accept(1), 21);
  for (bool bad_address : {false, true}) {
    f.Invite(0, 1);
    f.Failure(f.Accept(1, 0, bad_address ? 1 : 0, bad_address ? 1 : 0), 21);
    f.Failure(f.Accept(1), 21);
    f.Roster(0, {0});
    f.Roster(1, {1});
    f.Empty();
  }
  f.Invite(0, 1);
  auto joined = f.Accept(1);
  Check(joined.moved_to_menu && joined.responses.empty());
  f.Scene(1, 1, 1);
  f.Joined(0, {0, 1});
  f.Joined(1, {0, 1});
  f.Failure(f.Accept(1), 21);
  f.Invite(0, 1);
  f.Failure(f.Accept(1), 2);
  f.Empty();
  // Approval by the named owner is required; C111 cannot decline a visit.
  Sequence(
      f.leases[2].RequestVisit({Fixture::Character(1).character_name}, f.now),
      {});
  auto asked = f.queues[1]->Drain();
  Sequence(asked, {124});
  key = proto::AskedVisitationNotification::Decode(asked[0]).visitation_key;
  f.leases[0].ApproveVisit(key, f.now);
  f.leases[2].ApproveVisit(key, f.now);
  f.leases[2].Decline(key, f.now);
  f.Failure(f.Accept(2, 1, 0), 21);
  f.Empty();
  f.leases[1].ApproveVisit(key, f.now);
  Sequence(f.queues[2]->Drain(), {125});
  Check(f.Accept(2, 1, 0).moved_to_menu);
  f.Scene(2, 1, 1);
  for (auto i : {0, 1, 2}) f.Joined(i, {0, 1, 2});
  f.Empty();
}
void InvalidatedTargets() {
  for (int change = 0; change < 5; ++change) {
    Fixture f;
    f.Invite(0, 1);
    if (change == 0) f.leases[0].Invalidate();
    if (change == 1) f.leases[1].Invalidate();
    if (change == 2) f.members[0].Update(Fixture::Character(0), true);
    if (change == 3) f.Plaza(0);
    if (change == 4) {
      f.leases[0].Release();
      f.members[0].Release();
      f.members[0] =
          f.parties->Enter(Fixture::Character(0), f.queues[0]->Sink());
      f.leases[0] = f.invitations->Enter(Fixture::Character(0),
                                         f.queues[0]->Sink(), true);
    }
    f.Clear();
    f.Failure(f.Accept(1), 21);
    f.Roster(0, {0});
    f.Roster(1, {1});
    f.Empty();
  }
}
void AtomicScenes() {
  Fixture f;
  f.Plaza(0);
  f.Plaza(1);
  f.Clear();
  const auto old = f.locations[1].Token();
  f.Invite(0, 1);
  auto joined = f.Accept(1);
  Check(joined.plaza && joined.plaza.Token() != old);
  f.locations[1] = std::move(joined.plaza);
  Check(f.plazas->Capture(Fixture::Character(1).account_id)->token ==
        f.locations[1].Token());
  f.Scene(1, 3, 2);
  Sequence(f.scenes[0]->Drain(), {511, 503, 501});
  f.Joined(0, {0, 1});
  f.Joined(1, {0, 1});
  // An individual's acceptance departs its old party without dragging peers.
  f.Invite(2, 0);
  Check(f.Accept(0, 2).moved_to_menu);
  f.locations[0].Release();
  f.Scene(0, 1, 1);
  f.Joined(0, {2, 0});
  f.Joined(2, {2, 0});
  f.Roster(1, {1});
  Sequence(f.queues[1]->Drain(), {201, 203});
  Sequence(f.scenes[1]->Drain(), {511, 501});
  f.Clear();
  Fixture full;
  for (std::size_t i = 0; i < 17; ++i) full.Plaza(i);
  full.Clear();
  const auto before = *full.plazas->Capture(Fixture::Character(16).account_id);
  full.Invite(0, 16);
  full.Failure(full.Accept(16), 21);
  const auto after = *full.plazas->Capture(Fixture::Character(16).account_id);
  Check(before.scene == after.scene && before.token == after.token);
  full.Roster(0, {0});
  full.Roster(16, {16});
  full.Empty();
}
void RollbackAndIdentity() {
  for (bool plaza : {false, true}) {
    Fixture f;
    if (plaza) {
      f.Plaza(0);
      f.Plaza(1);
      f.Clear();
    }
    const auto old = f.locations[1].Token();
    f.Invite(0, 1);
    auto input = f.Input(1);
    auto bad = Fixture::Occupant(1);
    bad.equipment.groups[0].resize(1025);
    Rejects([&] {
      f.leases[1].Accept({u"PeerA", 0, 0, 1}, bad, f.scenes[1]->Sink(), f.now,
                         &input);
    });
    f.Roster(0, {0});
    f.Roster(1, {1});
    f.Empty();
    Check(f.locations[1].Token() == old);
    f.Failure(f.Accept(1), 21);
    f.Invite(0, 1);
    auto foreign = std::make_shared<parties::PartyRegistry>(8);
    auto identity = foreign->Enter(Fixture::Character(1), f.queues[1]->Sink());
    f.Clear();
    input.membership = &identity;
    f.Failure(f.leases[1].Accept({u"PeerA", 0, 0, 1}, Fixture::Occupant(1),
                                 f.scenes[1]->Sink(), f.now, &input),
              21);
    f.Roster(0, {0});
    f.Roster(1, {1});
    f.Empty();
  }
  Fixture f;
  auto target = *f.parties->Capture(u"PeerA");
  Check(f.members[1].JoinApproved(target, [] { return false; }) ==
        parties::PartyJoinStatus::kStale);
  Rejects([&] {
    f.members[1].JoinApproved(
        target, []() -> bool { throw std::runtime_error("scene failure"); });
  });
  f.Roster(0, {0});
  f.Roster(1, {1});
  f.Empty();
  f.Invite(0, 1);
  Check(f.Accept(1).moved_to_menu);
  f.Clear();
  f.Invite(0, 2);
  f.blocked = {Fixture::Character(1).character_id,
               Fixture::Character(2).character_id};
  f.Failure(f.Accept(2), 21);
  f.Roster(0, {0, 1});
  f.Roster(2, {2});
  f.Empty();
  f.blocked.reset();
  f.Invite(0, 2);
  f.members[2].Update(Fixture::Character(2), true);
  f.Failure(f.Accept(2), 8);
  f.Empty();
}
void LastSeat() {
  for (int repeat = 0; repeat < 10; ++repeat) {
    Fixture f(2);
    f.Invite(0, 1);
    f.Invite(0, 2);
    social::InvitationAdmission first, second;
    std::thread a([&] { first = f.Accept(1); });
    std::thread b([&] { second = f.Accept(2); });
    a.join();
    b.join();
    Check(first.moved_to_menu != second.moved_to_menu);
    auto winner = first.moved_to_menu ? 1 : 2;
    f.Failure(first.moved_to_menu ? std::move(second) : std::move(first), 14);
    f.Joined(0, {0, static_cast<std::size_t>(winner)});
    f.Joined(winner, {0, static_cast<std::size_t>(winner)});
    f.Scene(winner, 1, 1);
    f.Roster(3 - winner, {static_cast<std::size_t>(3 - winner)});
    f.Empty();
  }
}
}  // namespace
int RunPartyConsentTests() {
  Consent();
  InvalidatedTargets();
  AtomicScenes();
  RollbackAndIdentity();
  LastSeat();
  std::cout << "PASS: party consent, " << checks << " checks.\n";
  return checks;
}
