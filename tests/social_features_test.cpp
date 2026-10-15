#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>

#include "features/social/social_registry.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::tests {
namespace {
using namespace features;
using namespace protocol;
struct SocialFixture {
  std::map<std::uint64_t, social::SocialSnapshot> snapshots;
  CompanionSocialStatus permission = CompanionSocialStatus::kOk;
  bool fail = false;
};
class FixtureStore final : public social::SocialStore {
 public:
  explicit FixtureStore(std::shared_ptr<SocialFixture> data)
      : data_(std::move(data)) {}
  social::SocialSnapshot Load(
      const characters::CharacterRecord& owner) override {
    if (data_->fail) {
      throw std::runtime_error("Injected social persistence failure.");
    }
    return data_->snapshots.at(owner.character_id);
  }
  std::optional<ChatBlockEntry> Find(std::uint32_t id,
                                     std::u16string_view name) override {
    for (const auto& [key, s] : data_->snapshots) {
      if ((!id || key == id) &&
          (name.empty() || s.owner.character_name == name)) {
        return ChatBlockEntry{static_cast<std::uint32_t>(key),
                              s.owner.character_name};
      }
    }
    return {};
  }
  social::SocialChange Change(const characters::CharacterRecord&,
                              const CompanionSocialRequest&) override {
    return {CompanionSocialStatus::kUnavailable, {}};
  }
  CompanionSocialStatus CanMessage(const characters::CharacterRecord&,
                                   std::uint32_t) override {
    return data_->permission;
  }

 private:
  std::shared_ptr<SocialFixture> data_;
};
}  // namespace
std::size_t RunSocialFeaturesTests(const std::filesystem::path& fixtures) {
  std::size_t checks = 0;
  const auto check = [&](bool ok, const std::source_location at =
                                      std::source_location::current()) {
    ++checks;
    if (!ok) {
      throw std::runtime_error("Social feature line " +
                               std::to_string(at.line()));
    }
  };
  const auto rejects = [&](const auto& action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected);
  };
  const auto packet_check = [&]<class T>(const TcpPacket& packet) {
    check(T::Decode(packet).Encode().payload == packet.payload);
    for (std::size_t i = 0; i < packet.payload.size(); ++i) {
      auto truncated = packet;
      truncated.payload.resize(i);
      rejects([&] { T::Decode(truncated); });
    }
    auto extra = packet;
    extra.payload.push_back(0);
    rejects([&] { T::Decode(extra); });
    extra = packet;
    extra.message_id = 0;
    rejects([&] { T::Decode(extra); });
  };
  std::ifstream input(fixtures / "social_block_packets.txt");
  check(input.good());
  std::size_t vectors = 0;
  for (std::string line; std::getline(input, line);) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto p = line.find('|');
    TcpPacket packet{static_cast<std::uint16_t>(std::stoul(line.substr(0, p))),
                     {}};
    for (auto i = p + 1; i < line.size(); i += 2) {
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(line.substr(i, 2), nullptr, 16)));
    }
    if (packet.message_id == 147 || packet.message_id == 148) {
      packet_check.operator()<ChatBlockRequest>(packet);
    } else if (packet.message_id == 171) {
      packet_check.operator()<ChatBlockAddedNotification>(packet);
    } else if (packet.message_id == 172) {
      packet_check.operator()<ChatBlockRemovedNotification>(packet);
    } else {
      check(packet.message_id == 170);
      packet_check.operator()<ChatBlockListNotification>(packet);
    }
    ++vectors;
  }
  check(vectors >= 8);
  rejects([&] { ChatBlockRequest::Decode({147, {0, 0, 0, 0, 0}}); });
  rejects([&] { ChatBlockListNotification{{{1, u"A"}, {1, u"B"}}}.Encode(); });
  rejects([&] { ChatBlockRequest{false, 0, u"A"}.Encode(); });
  // LOCAL companion protocol cases. They are deliberately separate from native
  // vectors.
  for (int action = 0; action <= 8; ++action) {
    packet_check.operator()<CompanionSocialRequest>(CompanionSocialRequest{
        static_cast<CompanionSocialAction>(action), 42, u"角色", u"hello"}
                                                        .Encode());
  }
  packet_check.operator()<CompanionSocialRosterNotification>(
      CompanionSocialRosterNotification{
          CompanionSocialStatus::kOk,
          CompanionSocialAction::kList,
          {},
          {{42, u"角色", 1, true}, {43, u"离线", 4, false}}}
          .Encode());
  packet_check.operator()<CompanionPrivateMessageNotification>(
      CompanionPrivateMessageNotification{42, u"角色", u"发送者", u"私聊 😀"}
          .Encode());
  auto malformed = CompanionSocialRequest{}.Encode();
  malformed.payload[0] = 2;
  rejects([&] { CompanionSocialRequest::Decode(malformed); });
  malformed = CompanionSocialRequest{}.Encode();
  malformed.payload[4] = 9;
  rejects([&] { CompanionSocialRequest::Decode(malformed); });
  rejects([&] {
    CompanionPrivateMessageNotification{42, u"A", u"B",
                                        std::u16string(257, u'x')}
        .Encode();
  });
  auto data = std::make_shared<SocialFixture>();
  characters::CharacterRecord a{101, 1, u"Alice", 27, 2},
      b{102, 2, u"Bob", 27, 2}, c{103, 3, u"Carol", 27, 2};
  data->snapshots[101] = {a, {{102, u"Bob", 1, false}}, {}};
  data->snapshots[102] = {b, {{101, u"Alice", 1, false}}, {}};
  data->snapshots[103] = {c, {}, {}};
  auto registry = std::make_shared<social::SocialRegistry>(
      [data] { return std::make_unique<FixtureStore>(data); });
  auto qa = std::make_shared<rooms::RoomPacketQueue>(),
       qb = std::make_shared<rooms::RoomPacketQueue>(),
       qc = std::make_shared<rooms::RoomPacketQueue>();
  auto la = registry->Enter(a, qa->Sink());
  auto lb = registry->Enter(b, qb->Sink());
  auto lc = registry->Enter(c, qc->Sink());
  auto packets = qa->Drain();
  check(packets.size() == 1 && packets[0].message_id == 170);
  check(qb->Drain().size() == 1 && qc->Drain().size() == 1);
  rejects([&] { registry->Enter(a, qa->Sink()); });
  la.Process({});
  packets = qa->Drain();
  check(packets.size() == 1);
  check(CompanionSocialRosterNotification::Decode(packets[0]).rows[0].online);
  la.Process({CompanionSocialAction::kMessage, 102, {}, u"unavailable"});
  check(CompanionSocialRosterNotification::Decode(qa->Drain()[0]).status ==
        CompanionSocialStatus::kOffline);
  check(qb->Drain().empty());
  lb.Process({});
  qb->Drain();
  la.Process({CompanionSocialAction::kMessage, 102, {}, u"Hello Bob"});
  auto delivered =
      CompanionPrivateMessageNotification::Decode(qb->Drain().at(0));
  auto echoed = CompanionPrivateMessageNotification::Decode(qa->Drain().at(0));
  check(delivered.peer_id == 101 && delivered.speaker == u"Alice" &&
        delivered.text == u"Hello Bob");
  check(echoed.peer_id == 102 && echoed.speaker == u"Alice" &&
        echoed.text == delivered.text);
  check(qc->Drain().empty());
  data->permission = CompanionSocialStatus::kBlocked;
  la.Process({CompanionSocialAction::kMessage, 102, {}, u"blocked"});
  check(CompanionSocialRosterNotification::Decode(qa->Drain()[0]).status ==
        CompanionSocialStatus::kBlocked);
  check(qb->Drain().empty() && qc->Drain().empty());
  data->snapshots[102].blocks = {{101, u"Alice"}};
  lb.RefreshName();
  qb->Drain();
  qa->Drain();
  check(!registry->AllowsPublicChat(101, 102));
  check(registry->AllowsPublicChat(102, 101));
  check(registry->AllowsPublicChat(101, 101) &&
        registry->AllowsPublicChat(101, 103));
  data->snapshots[102].blocks.clear();
  data->snapshots[102].owner.character_name = u"Bobby";
  lb.RefreshName();
  qb->Drain();
  packets = qa->Drain();
  check(
      CompanionSocialRosterNotification::Decode(packets.back()).rows[0].name ==
      u"Bobby");
  data->permission = CompanionSocialStatus::kOk;
  la.Process({CompanionSocialAction::kMessage, 102, u"Bob", u"stale"});
  check(CompanionSocialRosterNotification::Decode(qa->Drain()[0]).status ==
        CompanionSocialStatus::kNotFound);
  la.Process({CompanionSocialAction::kMessage, 102, {}, u"\nforged"});
  check(CompanionSocialRosterNotification::Decode(qa->Drain()[0]).status ==
            CompanionSocialStatus::kInvalid &&
        qb->Drain().empty());
  lb.Release();
  packets = qa->Drain();
  check(!CompanionSocialRosterNotification::Decode(packets.back())
             .rows[0]
             .online);
  la.Release();
  lc.Release();
  data->fail = true;
  rejects([&] { registry->Enter(b, qb->Sink()); });
  data->fail = false;
  lb = registry->Enter(b, qb->Sink());
  check(static_cast<bool>(lb));
  std::cout << "Social feature tests: " << checks << " checks passed.\n";
  return checks;
}
}  // namespace bubble_fighter::server::tests
