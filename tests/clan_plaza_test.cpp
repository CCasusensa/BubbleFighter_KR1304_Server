#include <array>
#include <future>
#include <iostream>
#include <source_location>

#include "features/lobby/plaza_registry.h"
#include "protocol/scenes/scene_descriptor_notification.h"
#include "sessions/menu_snapshot.h"

namespace {
namespace bf = bubble_fighter::server;
namespace c = bf::features::clans;
namespace l = bf::features::lobby;
namespace p = bf::protocol;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan plaza line " + std::to_string(at.line()));
}
template <class F>
void Rejects(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  Check(failed);
}
void Authority() {
  auto auth = std::make_shared<c::ClanAuthentication>();
  std::optional<std::vector<c::ClanPlazaMember>> rows;
  int reads = 0, commits = 0;
  c::ClanPlazaAccess gate(auth, [&](auto) {
    ++reads;
    return rows;
  });
  struct Case {
    std::vector<std::uint32_t> clans, roles;
    std::uint32_t target, category;
    bool allowed;
  };
  // Policy cases were independently fixed before the business code.
  const std::vector<Case> cases{{{42}, {100}, 42, 60, true},
                                {{42}, {101}, 42, 60, true},
                                {{42}, {102}, 42, 60, true},
                                {{42, 42, 42}, {101, 100, 102}, 42, 60, true},
                                {{42, 0}, {101, 0}, 42, 60, false},
                                {{42, 43}, {101, 100}, 42, 60, false},
                                {{42, 42}, {101, 1}, 42, 60, false},
                                {{42}, {100}, 43, 60, false},
                                {{42}, {100}, 0, 60, false},
                                {{99, 99}, {100, 100}, 99, 60, true},
                                {{99}, {101}, 99, 60, false},
                                {{99}, {100}, 99, 80, false},
                                {{42, 42}, {101, 100}, 42, 80, true}};
  for (const auto& test : cases) {
    std::vector<c::ClanIdentity> ids;
    rows.emplace();
    commits = 0;
    for (std::uint32_t i = 0; i < test.clans.size(); ++i) {
      ids.push_back({100 + i, 10 + i});
      rows->push_back(
          {{ids.back(), 20, test.clans[i], test.roles[i], true}, u"클랜"});
    }
    const auto result = gate.Authorize(
        test.target, test.category, 1, 260, ids, [&](const auto& scope) {
          ++commits;
          Check(scope.clan_id == test.target && scope.name == u"클랜");
          return true;
        });
    Check(result.allowed == test.allowed);
    Check(commits == (test.allowed ? 1 : 0));
  }
  const std::array<c::ClanIdentity, 2> ids{{{100, 10}, {101, 11}}};
  const std::vector<c::ClanPlazaMember> valid{
      {{ids[0], 20, 42, 101, true}, u"클랜"},
      {{ids[1], 20, 42, 100, true}, u"클랜"}};
  const auto deny = [&] {
    commits = 0;
    Check(!gate.Authorize(42, 60, 1, 260, ids,
                          [&](const auto&) {
                            ++commits;
                            return true;
                          })
               .allowed);
    Check(commits == 0);
  };
  rows.reset();
  deny();
  rows.emplace();
  deny();
  for (int fault = 0; fault < 13; ++fault) {
    rows = valid;
    auto& row = rows->back();
    switch (fault) {
      case 0:
        row.member.identity.account_id = 999;
        break;
      case 1:
        row.member.identity.character_id = 999;
        break;
      case 2:
        row.member.enabled = false;
        break;
      case 3:
        row.member.level = 0;
        break;
      case 4:
        row.member.level = 261;
        break;
      case 5:
        row.member.clan_id = 43;
        break;
      case 6:
        row.member.membership_status = 99;
        break;
      case 7:
        row.member.membership_status = 103;
        break;
      case 8:
        row.clan_name = u"Different";
        break;
      case 9:
        rows->front().clan_name.clear();
        break;
      case 10:
        rows->front().clan_name = std::u16string(13, u'A');
        break;
      case 11:
        rows->front().clan_name = u"Bad Name";
        break;
      case 12:
        rows->push_back(row);
        break;
    }
    deny();
  }
  rows = valid;
  auto before = reads;
  for (const auto& bad : std::vector<std::vector<c::ClanIdentity>>{
           {},
           {{0, 10}},
           {{100, 0}},
           {{100, 10}, {100, 11}},
           {{100, 10}, {101, 10}},
           std::vector<c::ClanIdentity>(9, {100, 10})})
    Check(!gate.Authorize(42, 60, 1, 260, bad).allowed);
  Check(!gate.Authorize(0x80000000U, 60, 1, 260, ids).allowed);
  Check(!gate.Authorize(42, 61, 1, 260, ids).allowed);
  Check(!gate.Authorize(42, 60, 0, 260, ids).allowed);
  Check(!gate.Authorize(42, 60, 30, 20, ids).allowed);
  Check(!gate.Authorize(42, 60, 1, 261, ids).allowed);
  Check(reads == before);
  Check(!gate.Authorize(42, 60, 21, 260, ids).allowed);
  Check(!gate.Authorize(42, 60, 1, 19, ids).allowed);
  for (auto category : {20U, 60U, 70U, 80U})
    Check(gate.Authorize(42, category, 20, 20, ids).allowed);
  Check(!gate.Authorize(42, 60, 1, 260, ids, [](const auto&) { return false; })
             .allowed);
  Rejects([&] {
    gate.Authorize(42, 60, 1, 260, ids, [](const auto&) -> bool {
      throw std::runtime_error("Synthetic clan scene failure");
    });
  });
  Check(gate.Authorize(42, 60, 1, 260, ids).allowed);
  // The shared operations lock must span the final scene commit.
  std::promise<void> entered, release, writer_started;
  auto wait = release.get_future().share();
  auto admission = std::async(std::launch::async, [&] {
    return gate.Authorize(42, 60, 1, 260, ids, [&](const auto&) {
      entered.set_value();
      wait.wait();
      return true;
    });
  });
  entered.get_future().wait();
  auto writer = std::async(std::launch::async, [&] {
    writer_started.set_value();
    auto lock = auth->LockOperations();
    auth->InvalidateMembership();
  });
  writer_started.get_future().wait();
  const bool blocked = writer.wait_for(std::chrono::milliseconds(30)) ==
                       std::future_status::timeout;
  release.set_value();
  const auto result = admission.get();
  writer.get();
  Check(blocked && result.allowed && gate.Revision() != result.revision);
  Rejects([&] { c::ClanPlazaAccess bad(nullptr, [&](auto) { return rows; }); });
  Rejects([&] { c::ClanPlazaAccess bad(auth, {}); });
}
struct Registry {
  std::shared_ptr<l::PlazaRegistry> plazas =
      std::make_shared<l::PlazaRegistry>();
  std::array<std::shared_ptr<bf::features::rooms::RoomPacketQueue>, 20> queues;
  std::array<l::PlazaLease, 20> leases;
  Registry() {
    for (auto& q : queues)
      q = std::make_shared<bf::features::rooms::RoomPacketQueue>();
  }
  static bf::features::characters::CharacterRecord Character(std::size_t i) {
    return {10 + i, 100 + i,
            u"Fighter" + std::u16string(1, static_cast<char16_t>(u'A' + i)), 27,
            2};
  }
  static bf::sessions::MenuInitializationData Data(std::size_t i) {
    const auto v = Character(i);
    return bf::sessions::CreateMenuSnapshot(
        v, {v.account_id, v.character_id, {}}, 1000);
  }
  l::PlazaEntry Entry(std::size_t i) {
    auto d = Data(i);
    return {{d.member, Character(i), d.equipment},
            100 + i,
            queues[i]->Sink(),
            leases[i].Token()};
  }
  l::PlazaTarget Target(std::size_t i) { return *plazas->Capture(100 + i); }
  l::PlazaAdmission Enter(std::size_t i, c::ClanPlazaScope scope = {},
                          std::optional<l::PlazaTarget> target = {}) {
    auto d = Data(i);
    d.scene.descriptor.wide_at80 = 999999;
    return plazas->EnterGroup(d.scene, {Entry(i)}, target, std::move(scope));
  }
  void Adopt(std::size_t i, l::PlazaAdmission value) {
    Check(value.status == l::PlazaAdmissionStatus::kEntered &&
          value.members.size() == 1);
    leases[i] = std::move(value.members[0]);
  }
  void Clear() {
    for (auto& q : queues) q->Clear();
  }
};
void Partition() {
  Registry f;
  f.Adopt(0, f.Enter(0));
  f.Adopt(1, f.Enter(1, {42, u"클랜"}));
  f.Adopt(2, f.Enter(2, {43, u"Other"}));
  f.Adopt(3, f.Enter(3, {42, u"클랜"}));
  Check(f.Target(0).scene != f.Target(1).scene &&
        f.Target(1).scene != f.Target(2).scene);
  Check(f.Target(1).scene == f.Target(3).scene && f.Target(1).clan_id == 42);
  for (auto i : {1U, 3U}) {
    const auto packets = f.queues[i]->Drain();
    Check(!packets.empty() && packets.front().message_id == 117);
    const auto scene = p::GotoRoomNotification::Decode(packets.front());
    Check(scene.descriptor.session_type == 6 &&
          scene.descriptor.text_at88 == u"클랜" &&
          scene.descriptor.wide_at80 == UINT64_MAX &&
          f.leases[i].ClanId() == 42);
  }
  const auto source = f.Target(0);
  f.Clear();
  Check(f.Enter(0, {}, f.Target(1)).status == l::PlazaAdmissionStatus::kStale);
  Check(f.Enter(0, {43, u"Other"}, f.Target(1)).status ==
        l::PlazaAdmissionStatus::kStale);
  auto forged = f.Target(1);
  forged.clan_id = 0;
  Check(f.Enter(0, {}, forged).status == l::PlazaAdmissionStatus::kStale);
  Check(f.Target(0).token == source.token);
  for (auto& q : f.queues) Check(q->Drain().empty());
  p::PlazaMovementRequest move;
  move.sequence = 1;
  f.leases[1].Move(move);
  Check(f.queues[3]->Drain().size() == 1);
  Check(f.queues[0]->Drain().empty() && f.queues[2]->Drain().empty());
  f.Clear();
  bf::features::chat::SceneChat::Message chat;
  chat.text = u"Clan only";
  f.leases[1].Chat(chat);
  Check(f.queues[1]->Drain().size() == 1 && f.queues[3]->Drain().size() == 1);
  Check(f.queues[0]->Drain().empty() && f.queues[2]->Drain().empty());
  auto data = Registry::Data(1);
  f.leases[1].Refresh({data.member, Registry::Character(1), data.equipment},
                      true);
  auto own = f.queues[1]->Drain();
  Check(!own.empty() &&
        p::GotoRoomNotification::Decode(own.front()).descriptor.session_type ==
            6);
  f.Clear();
  // A fresh canonical name updates the descriptor for existing private peers.
  f.Adopt(4, f.Enter(4, {42, u"Renamed"}, f.Target(1)));
  auto updates = f.queues[1]->Drain();
  Check(!updates.empty() && updates.back().message_id == 501);
  auto descriptor = p::SceneDescriptorNotification::Decode(updates.back());
  Check(descriptor.descriptor.session_type == 6 &&
        descriptor.descriptor.text_at88 == u"Renamed");
  const auto stale = f.Target(1);
  auto old = std::move(f.leases[1]);
  auto entry = f.Entry(1);
  entry.source_token = old.Token();
  auto result = f.plazas->EnterGroup(data.scene, {entry}, {}, {42, u"Renamed"});
  f.Adopt(1, std::move(result));
  old.Release();
  Check(f.Target(1).token != stale.token && f.leases[1].ClanId() == 42);
  Check(f.Enter(0, {42, u"Renamed"}, stale).status ==
        l::PlazaAdmissionStatus::kStale);
  f.Adopt(1, f.Enter(1));
  Check(f.leases[1].ClanId() == 0 && f.Target(1).scene == f.Target(0).scene);
  Rejects([&] { f.Enter(5, {0, u"Invalid"}); });
  Rejects([&] { f.Enter(5, {42, u""}); });
}
void Capacity() {
  Registry f;
  for (std::size_t i = 0; i < 16; ++i) f.Adopt(i, f.Enter(i, {42, u"Clan"}));
  f.Adopt(16, f.Enter(16, {43, u"Other"}));
  const auto before = f.Target(16);
  f.Clear();
  Check(f.Enter(16, {42, u"Clan"}, f.Target(0)).status ==
        l::PlazaAdmissionStatus::kFull);
  Check(f.Target(16).token == before.token);
  for (auto& q : f.queues) Check(q->Drain().empty());
  f.Adopt(17, f.Enter(17, {42, u"Clan"}));
  Check(f.Target(17).scene != f.Target(0).scene);
  auto bad = f.Entry(18);
  bad.player.state.values_at52.resize(4097);
  Rejects([&] {
    f.plazas->EnterGroup(Registry::Data(18).scene, {bad}, {}, {42, u"Clan"});
  });
  Check(!f.plazas->Capture(118));
  Check(f.Target(16).token == before.token);
}
}  // namespace
int RunClanPlazaTests() {
  Authority();
  Partition();
  Capacity();
  std::cout << "Clan plaza authority/partition: " << checks
            << " checks passed.\n";
  return checks;
}
