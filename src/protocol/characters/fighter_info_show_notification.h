#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_SHOW_NOTIFICATION_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_SHOW_NOTIFICATION_H_

#include <cstdint>
#include <array>

#include "protocol/codec/tcp_packet_codec.h"
#include "protocol/characters/fighter_info.h"

namespace bubble_fighter::server::protocol {
// E070: the client creates DlgFighterInfo (战士信息) from this message, and its
// reader 0x0099A430 reads the body in this exact order:
//
//   u32 value_at0,  FighterInfo, FighterInfo,  u32 x4
//
// value_at0 must be 1: 0x009BCE60 shows its own "异常玩家" box (string key
// 0x015B594C) for every other value, so an empty body is NOT acceptable. The
// two FighterInfo blocks are the same 0x009D8030/0x009A5C40 record S105 carries;
// the trailing four 32-bit fields stay an explicit zero baseline.
struct FighterInfoShowNotification {
  static constexpr std::uint16_t kMessageId = 0x0085;
  static constexpr std::uint32_t kValidUser = 1;

  std::uint32_t value_at0 = kValidUser;
  FighterInfo target;
  FighterInfo self;
  std::array<std::uint32_t, 4> values_at_end{};

  TcpPacket Encode() const;
  static FighterInfoShowNotification Decode(const TcpPacket& packet);
  bool operator==(const FighterInfoShowNotification&) const = default;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_INFO_SHOW_NOTIFICATION_H_
