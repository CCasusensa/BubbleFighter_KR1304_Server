#include <array>
#include <barrier>
#include <future>
#include <iostream>
#include <source_location>

#include "features/clans/clan_master_transfers.h"
#include "sessions/clan_session.h"

namespace {
namespace clans = bubble_fighter::server::features::clans;
namespace proto = bubble_fighter::server::protocol;
using Auth = clans::ClanAuthentication;
using Session = bubble_fighter::server::sessions::ClanSession;
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Clan master transfer line " +
                             std::to_string(at.line()));
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
struct Fixture {
  Auth::Clock::time_point now{};
  std::shared_ptr<Auth> auth = std::make_shared<Auth>([this] { return now; });
  std::array<clans::ClanIdentity, 3> ids{{{1, 10}, {2, 20}, {3, 30}}};
  std::array<Auth::GameLink, 3> games;
  std::array<Auth::Binding, 3> bindings;
  std::array<std::u16string, 3> names{u"Master", u"Target", u"Third"};
  bool eligible = true, throw_commit = false, revoke_commit = false;
  int master = 0, reads = 0, commits = 0;
  std::shared_ptr<clans::ClanMasterTransfers> service;
  Fixture() {
    for (std::size_t i = 0; i < ids.size(); ++i) {
      games[i] = auth->BindGame(ids[i]);
      auth->UpdateScene(games[i], {60, 3});
      bindings[i] = auth->Consume(auth->Issue(games[i], names[i]));
    }
    service = std::make_shared<clans::ClanMasterTransfers>(
        auth,
        [this](const auto& actor, std::uint32_t target)
            -> std::optional<clans::ClanMasterTransferParties> {
          ++reads;
          if (!eligible || actor != ids[master] || target == actor.character_id)
            return {};
          for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i].character_id == target)
              return clans::ClanMasterTransferParties{
                  actor, ids[i], 7000, names[master], names[i], 100};
          return {};
        },
        [this](const auto& parties, const auto& authorized) {
          ++commits;
          if (revoke_commit) auth->UpdateScene(games[1], {60, 0});
          if (throw_commit || !authorized())
            throw std::runtime_error("Synthetic commit failure");
          master = parties.target == ids[1] ? 1 : 2;
          return clans::ClanMutationResult{
              {proto::ClanMasterTransferredNotification{
                  0, parties.requester.character_id, parties.requester_name,
                  parties.target.character_id, parties.target_name}
                   .Encode()},
              {}};
        },
        [this](const auto& id) { return Profile(id); }, [this] { return now; });
  }
  std::optional<clans::ClanSnapshot> Profile(const clans::ClanIdentity& id) {
    clans::ClanSnapshot snapshot;
    snapshot.profile.clan_id = 7000;
    snapshot.profile.clan_name = u"Clan";
    snapshot.profile.membership_status = id == ids[master] ? 101 : 100;
    return snapshot;
  }
  auto Send(std::size_t who, const clans::ClanMasterTransferAction& action) {
    auto result = service->Process(bindings[who], action);
    auth->Publish(result.deliveries);
    return result;
  }
  std::uint32_t Offer(std::size_t target = 1) {
    auto result =
        Send(0, proto::ClanMasterTransferRequest{ids[target].character_id});
    Check(proto::ClanMasterTransferResponse::Decode(result.responses.at(0))
              .available == 1);
    auto notices = bindings[target].TakeNotifications();
    Check(notices && notices->size() == 1);
    auto offer =
        proto::ClanMasterTransferOfferNotification::Decode(notices->at(0));
    Check(offer.requester_name == names[0] && offer.correlation_value);
    return offer.correlation_value;
  }
  void Answer(std::uint32_t key, bool accept = true, std::size_t target = 1) {
    auto result = Send(target, proto::ClanMasterTransferAnswerRequest{
                                   static_cast<std::uint8_t>(accept), key});
    Check(
        proto::ClanMasterTransferAnswerResponse::Decode(result.responses.at(0))
            .success == 1);
    auto notices = bindings[0].TakeNotifications();
    Check(notices && notices->size() == 1);
    Check(proto::ClanMasterTransferDecisionNotification::Decode(
              notices->at(0)) == proto::ClanMasterTransferDecisionNotification{
                                     static_cast<std::uint8_t>(accept),
                                     ids[target].character_id, names[target]});
    Check(commits == 0 && master == 0);
  }
  auto Commit(std::size_t target = 1) {
    return Send(0, proto::ClanMasterTransferCommitRequest{
                       ids[target].character_id, names[target]});
  }
  void FailedCommit() {
    const auto result = Commit();
    Check(result.deliveries.empty() && result.responses.size() == 2);
    Check(proto::ClanMasterTransferResponse::Decode(result.responses[0])
              .available == 0);
    Check(proto::ClanProfileResponse::Decode(result.responses[1])
              .membership_status == 101);
    Check(commits == 0 && master == 0);
  }
};
void Consent() {
  Fixture f;
  f.FailedCommit();
  auto missing = f.Send(1, proto::ClanMasterTransferAnswerRequest{1, 1});
  Check(proto::ClanMasterTransferAnswerResponse::Decode(missing.responses[0])
            .success == 0);
  const auto key = f.Offer();
  auto duplicate = f.Send(0, proto::ClanMasterTransferRequest{30});
  Check(proto::ClanMasterTransferResponse::Decode(duplicate.responses[0])
            .available == 0);
  Check(f.bindings[2].TakeNotifications()->empty());
  auto wrong = f.Send(2, proto::ClanMasterTransferAnswerRequest{1, key});
  Check(proto::ClanMasterTransferAnswerResponse::Decode(wrong.responses[0])
            .success == 0);
  auto invalid = f.Send(1, proto::ClanMasterTransferAnswerRequest{255, key});
  Check(proto::ClanMasterTransferAnswerResponse::Decode(invalid.responses[0])
            .success == 0);
  f.Answer(key);
  Check(proto::ClanMasterTransferAnswerResponse::Decode(
            f.Send(1, proto::ClanMasterTransferAnswerRequest{1, key})
                .responses[0])
            .success == 0);
  auto committed = f.Commit();
  Check(committed.responses[0].message_id == 1056 && f.master == 1 &&
        f.commits == 1);
  auto replay = f.Commit();
  Check(replay.responses[0].message_id == 1058 && f.commits == 1);
  Check(proto::ClanProfileResponse::Decode(replay.responses[1])
            .membership_status == 100);
  Fixture reject;
  const auto rejected = reject.Offer();
  reject.Answer(rejected, false);
  reject.FailedCommit();
  Fixture early;
  const auto premature = early.Offer();
  early.FailedCommit();
  Check(proto::ClanMasterTransferAnswerResponse::Decode(
            early.Send(1, proto::ClanMasterTransferAnswerRequest{1, premature})
                .responses[0])
            .success == 0);
  Fixture mismatch;
  auto accepted = mismatch.Offer();
  mismatch.Answer(accepted);
  auto bad =
      mismatch.Send(0, proto::ClanMasterTransferCommitRequest{20, u"Forged"});
  Check(bad.responses[0].message_id == 1058);
  mismatch.FailedCommit();
}
void Lifetimes() {
  for (int kind = 0; kind < 10; ++kind) {
    Fixture f;
    auto key = f.Offer();
    f.Answer(key);
    if (kind == 0)
      f.bindings[1] = {};  // Only the clan socket closes; main stays live.
    if (kind == 1) f.games[1].reset();
    if (kind == 2)
      f.bindings[1] = f.auth->Consume(f.auth->Issue(f.games[1], f.names[1]));
    if (kind == 3)
      f.bindings[0] = f.auth->Consume(f.auth->Issue(f.games[0], f.names[0]));
    if (kind == 4 || kind == 5) {
      const auto i = kind == 4 ? 0 : 1;
      f.auth->UpdateScene(f.games[i], {60, 0});
      f.auth->UpdateScene(f.games[i], {60, 3});
    }
    if (kind == 6) f.now += std::chrono::seconds(60);
    if (kind == 7) f.auth->UpdateScene(f.games[1], {60, 0});
    if (kind == 8) {
      f.bindings[0] = {};
      f.bindings[0] = f.auth->Consume(f.auth->Issue(f.games[0], f.names[0]));
    }
    if (kind == 9) {
      f.games[1].reset();
      f.games[1] = f.auth->BindGame(f.ids[1]);
      f.auth->UpdateScene(f.games[1], {60, 3});
      f.bindings[1] = f.auth->Consume(f.auth->Issue(f.games[1], f.names[1]));
    }
    f.FailedCommit();
  }
  Fixture observe;
  auto copied = observe.bindings[1];
  auto found = observe.auth->Find(20);
  Check(copied.SameConnection(found));
  observe.bindings[1] = {};
  Check(!copied.Identity() && !found.Identity() && !copied.PlazaEpoch());
  Check(!observe.auth->Find(20).Identity());
  Fixture changed;
  auto key = changed.Offer();
  changed.names[1] = u"Renamed";
  Check(proto::ClanMasterTransferAnswerResponse::Decode(
            changed.Send(1, proto::ClanMasterTransferAnswerRequest{1, key})
                .responses[0])
            .success == 0);
  changed.FailedCommit();
  Fixture forbidden;
  key = forbidden.Offer();
  forbidden.eligible = false;
  Check(proto::ClanMasterTransferAnswerResponse::Decode(
            forbidden.Send(1, proto::ClanMasterTransferAnswerRequest{1, key})
                .responses[0])
            .success == 0);
  forbidden.FailedCommit();
  for (int i = 0; i < 2; ++i) {
    Fixture f;
    f.auth->UpdateScene(f.games[i], {60, 0});
    Check(proto::ClanMasterTransferResponse::Decode(
              f.Send(0, proto::ClanMasterTransferRequest{20}).responses[0])
                  .available == 0 &&
          f.reads == 0);
  }
  Fixture offline;
  offline.bindings[1] = {};
  Check(proto::ClanMasterTransferResponse::Decode(
            offline.Send(0, proto::ClanMasterTransferRequest{20}).responses[0])
            .available == 0);
}
void FailuresAndCompetition() {
  for (bool revoke : {false, true}) {
    Fixture f;
    auto key = f.Offer();
    f.Answer(key);
    f.throw_commit = !revoke;
    f.revoke_commit = revoke;
    Rejects([&] { f.Commit(); });
    Check(f.master == 0 && f.commits == 1);
    f.throw_commit = false;
    f.revoke_commit = false;
    f.auth->UpdateScene(f.games[1], {60, 3});
    Check(f.Commit().responses[0].message_id == 1058 && f.commits == 1);
  }
  Fixture race;
  std::barrier start(2);
  const auto run = [&](std::uint32_t target) {
    start.arrive_and_wait();
    return race.Send(0, proto::ClanMasterTransferRequest{target});
  };
  auto a = std::async(std::launch::async, run, 20);
  auto b = std::async(std::launch::async, run, 30);
  const auto first =
      proto::ClanMasterTransferResponse::Decode(a.get().responses[0]).available;
  const auto second =
      proto::ClanMasterTransferResponse::Decode(b.get().responses[0]).available;
  Check(first + second == 1 && race.commits == 0);
  Check(race.bindings[1].TakeNotifications()->size() +
            race.bindings[2].TakeNotifications()->size() ==
        1);
}
void SessionRoutes() {
  Fixture f;
  auto profiles = [&](const auto& id) { return f.Profile(id); };
  Session sender(
      f.auth, profiles, [&] { return f.now; }, {}, {}, {}, {}, f.service);
  auto recipient = std::make_unique<Session>(
      f.auth, profiles, Auth::Clock::now, clans::ClanCreateHandler{},
      clans::ClanDirectorySource{}, clans::ClanRosterSource{},
      clans::ClanMutationHandler{}, f.service);
  sender.Process(
      proto::ClanLoginRequest{f.auth->Issue(f.games[0], f.names[0])}.Encode());
  recipient->Process(
      proto::ClanLoginRequest{f.auth->Issue(f.games[1], f.names[1])}.Encode());
  auto offered = sender.Process(proto::ClanMasterTransferRequest{20}.Encode());
  Check(offered.responses[0].message_id == 1058);
  auto offer = proto::ClanMasterTransferOfferNotification::Decode(
      recipient->Poll().responses.at(0));
  auto answered = recipient->Process(
      proto::ClanMasterTransferAnswerRequest{1, offer.correlation_value}
          .Encode());
  Check(answered.responses[0].message_id == 1061);
  // Pending decision precedes a new commit reply on the same connection.
  auto committed = sender.Process(
      proto::ClanMasterTransferCommitRequest{20, u"Target"}.Encode());
  Check(committed.responses.size() == 2 &&
        committed.responses[0].message_id == 1062 &&
        committed.responses[1].message_id == 1056);
  auto malformed = proto::ClanMasterTransferRequest{20}.Encode();
  malformed.payload.push_back(0);
  Rejects([&] { sender.Process(malformed); });
  Check(sender.Poll().close_connection);
  for (const auto& packet :
       {proto::ClanMasterTransferRequest{20}.Encode(),
        proto::ClanMasterTransferAnswerRequest{1, 1}.Encode(),
        proto::ClanMasterTransferCommitRequest{20, u"Target"}.Encode()}) {
    Session early(f.auth, profiles);
    Rejects([&] { early.Process(packet); });
    Session missing(f.auth, profiles);
    missing.Process(
        proto::ClanLoginRequest{f.auth->Issue(f.games[0], f.names[0])}
            .Encode());
    Rejects([&] { missing.Process(packet); });
  }
}
}  // namespace

int RunClanMasterTransferTests() {
  Consent();
  Lifetimes();
  FailuresAndCompetition();
  SessionRoutes();
  std::cout << "Clan master transfer consent: " << checks << " checks passed\n";
  return checks;
}
