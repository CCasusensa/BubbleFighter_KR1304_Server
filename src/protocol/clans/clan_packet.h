#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PACKET_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bubble_fighter::server::protocol {
// This independent connection has a four-byte header and a per-frame XOR
// transform. Keeping a separate type prevents use of the main TCP envelope.
struct ClanPacket {
  std::uint16_t message_id = 0;
  std::vector<std::uint8_t> payload;
  bool operator==(const ClanPacket&) const = default;
};
enum class ClanDecodeStatus { kComplete, kNeedMoreData, kPayloadTooLarge };
struct ClanDecodeResult {
  ClanDecodeStatus status = ClanDecodeStatus::kNeedMoreData;
  std::size_t frame_size = 4;
  ClanPacket packet;
};
class ClanPacketCodec {
 public:
  static constexpr std::size_t kHeaderSize = 4;
  // BFCLIENT02 11143A0 rejects payload lengths >= 0xFFFC.
  static constexpr std::size_t kMaxPayloadSize = 0xFFFB;
  static std::vector<std::uint8_t> Encode(const ClanPacket& packet);
  // Decode one encrypted frame; frame_size tells callers what was consumed.
  // The native transform has no integrity check and does not authenticate.
  static ClanDecodeResult Decode(std::span<const std::uint8_t> bytes);
};
class ClanStreamDecoder {
 public:
  std::vector<ClanPacket> Feed(std::span<const std::uint8_t> bytes);
  void Finish();
  bool IsFailed() const noexcept { return failed_; }
  bool HasPendingData() const noexcept { return !buffer_.empty(); }

 private:
  [[noreturn]] void Fail(const char* message);
  std::vector<std::uint8_t> buffer_;
  bool failed_ = false;
  bool finished_ = false;
};
}  // namespace bubble_fighter::server::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_PACKET_H_
