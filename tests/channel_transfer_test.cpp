#include "protocol/scenes/channel_transfer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "protocol/login/client_to_server_message_0x0001.h"
#include "protocol/login/login_request.h"
#include "protocol/scenes/move_into_response.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bf = bubble_fighter::server;
namespace proto = bf::protocol;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Channel transfer line " +
                             std::to_string(at.line()));
}
template <typename F>
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
    case 53:
      return proto::MoveChannelRequest::Decode(packet).Encode();
    case 54:
      return proto::MoveIntoRequest::Decode(packet).Encode();
    case 55:
      return proto::ChannelSwitchAcknowledgementRequest::Decode(packet)
          .Encode();
    case 8:
      return proto::MoveChannelResponse::Decode(packet).Encode();
    case 9:
      return proto::ChannelDisconnectingNotification::Decode(packet).Encode();
    case 10:
      return proto::ChannelDisconnectNotification::Decode(packet).Encode();
    case 11:
      return proto::MoveIntoResponse::Decode(packet).Encode();
    case 163:
      return proto::FirstChannelMoveNotification::Decode(packet).Encode();
    default:
      throw std::logic_error("Unexpected channel fixture");
  }
}
void Codecs(const std::filesystem::path& directory) {
  std::ifstream file(directory / "channel_transfer_packets.txt");
  Check(file.good());
  int rows = 0;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line[0] == '#') continue;
    ++rows;
    const auto first = line.find('|'), second = line.find('|', first + 1);
    const auto last = line.rfind('|');
    proto::TcpPacket packet{static_cast<std::uint16_t>(std::stoul(
                                line.substr(first + 1, second - first - 1))),
                            {}};
    for (auto i = last + 1; i < line.size(); i += 2)
      packet.payload.push_back(static_cast<std::uint8_t>(
          std::stoul(line.substr(i, 2), nullptr, 16)));
    Check(Recode(packet).message_id == packet.message_id &&
          Recode(packet).payload == packet.payload);
    for (std::size_t n = 0; n < packet.payload.size(); ++n) {
      auto truncated = packet;
      truncated.payload.resize(n);
      Rejects([&] { Recode(truncated); });
    }
    auto extra = packet;
    extra.payload.push_back(0);
    Rejects([&] { Recode(extra); });
    if (rows == 1)
      Check(proto::MoveChannelRequest::Decode(packet).channel_category == 60);
    if (rows == 4) {
      const auto message = proto::MoveIntoRequest::Decode(packet);
      Check(message.character_id == 0x12345678 &&
            message.unknown_ui_value == 0);
    }
    if (rows == 8 || rows == 9) {
      const auto message = proto::MoveChannelResponse::Decode(packet);
      Check(message.result == 0);
      Check(message.ipv4_address ==
            (rows == 8 ? std::array<std::uint8_t, 4>{192, 0, 2, 1}
                       : std::array<std::uint8_t, 4>{127, 0, 0, 1}));
      Check(message.port == (rows == 8 ? 0x1234 : 0x9abc));
      Check(message.channel_category == (rows == 8 ? 60U : 70U));
    }
    if (rows == 10)
      Check(proto::MoveChannelResponse::Decode(packet).result ==
            proto::MoveChannelResponse::kUnavailable);
    if (packet.message_id == 11)
      Check(proto::MoveIntoResponse::Decode(packet).IsAccepted() ==
            (rows != 16));
  }
  Check(rows == 18);
  Rejects([] { proto::MoveChannelRequest::Decode({52, {}}); });
  Rejects([] { proto::MoveIntoRequest::Decode({53, {}}); });
  Rejects([] { proto::MoveChannelResponse::Decode({7, {}}); });
  Rejects([] { proto::ChannelSwitchAcknowledgementRequest::Decode({54, {}}); });
  Rejects([] { proto::ChannelDisconnectingNotification::Decode({8, {}}); });
  Rejects([] { proto::ChannelDisconnectNotification::Decode({9, {}}); });
  Rejects([] { proto::FirstChannelMoveNotification::Decode({162, {}}); });
}

struct Characters final : bf::features::characters::CharacterStore {
  bf::features::characters::CharacterRecord record{17, 7, u"Alice", 10, 7};
  int reads = 0;
  std::optional<bf::features::characters::CharacterRecord> FindByAccount(
      std::uint64_t id) override {
    ++reads;
    return id == record.account_id ? std::optional{record} : std::nullopt;
  }
  bool NameExists(std::u16string_view) override { return false; }
  bf::features::characters::CharacterCreateResult Create(
      bf::features::characters::CharacterRecord&) override {
    throw std::logic_error("Unexpected create");
  }
};
void Authorization() {
  // All first-connection variants are rejected without consuming a ticket or
  // loading a character. A public character identifier is not a credential.
  for (const auto& packet :
       {proto::MoveChannelRequest{60}.Encode(),
        proto::MoveIntoRequest{17, 1}.Encode(),
        proto::ChannelSwitchAcknowledgementRequest{}.Encode()}) {
    for (bool hello_first : {false, true}) {
      auto store = std::make_shared<Characters>();
      int consumed = 0;
      bf::sessions::PlayerSession session(
          [&](std::string_view) -> std::optional<std::uint64_t> {
            ++consumed;
            return 7;
          },
          store);
      if (hello_first) {
        proto::ClientToServerMessage0x0001 hello;
        hello.initial_value = 1256;
        Check(session.Process(hello.Encode()).size() == 2);
      }
      Rejects([&] { session.Process(packet); });
      Check(session.State() == bf::sessions::PlayerSessionState::kRejected);
      Check(!session.AccountId() && !session.Character());
      Check(consumed == 0 && store->reads == 0);
      Rejects([&] { session.Process({521, {}}); });
    }
  }
  for (const auto& invalid :
       {proto::MoveIntoRequest{17, 0}.Encode(),
        proto::ChannelSwitchAcknowledgementRequest{}.Encode(),
        proto::TcpPacket{53, {60, 0, 0}}}) {
    auto store = std::make_shared<Characters>();
    int consumed = 0;
    bf::sessions::PlayerSession session(
        [&](std::string_view) -> std::optional<std::uint64_t> {
          ++consumed;
          return 7;
        },
        store,
        [](const auto& character, auto observed) {
          return bf::sessions::CreateMenuSnapshot(
              character, {character.account_id, character.character_id, {}},
              observed);
        });
    proto::ClientToServerMessage0x0001 hello;
    hello.initial_value = 1256;
    session.Process(hello.Encode());
    proto::LoginRequest login;
    login.nexon_passport = u"BFS1." + std::u16string(64, u'0');
    Check(session.Process(login.Encode()).size() == 2);
    session.Process({521, {}});
    for (auto category : {60U, 70U, 0xffffffffU}) {
      const auto response =
          session.Process(proto::MoveChannelRequest{category}.Encode());
      Check(response.size() == 1 && response[0].message_id == 8);
      const auto denied = proto::MoveChannelResponse::Decode(response[0]);
      Check(denied == proto::MoveChannelResponse{2, {}, 0, category});
      Check(session.AccountId() == 7 &&
            session.Character()->character_id == 17);
      Check(session.State() ==
            bf::sessions::PlayerSessionState::kMenuInitializationSent);
      Check(consumed == 1);
    }
    Rejects([&] { session.Process(invalid); });
    Check(consumed == 1 && !session.AccountId() && !session.Character());
  }
}
}  // namespace
int RunChannelTransferTests(const std::filesystem::path& directory) {
  checks = 0;
  Codecs(directory);
  Authorization();
  std::cout << "Channel codecs/authorization: " << checks << " checks\n";
  return checks;
}
