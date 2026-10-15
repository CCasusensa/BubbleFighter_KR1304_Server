#include "protocol/rooms/room_configuration_request.h"

#include <stdexcept>

#include "protocol/codec/packet_reader.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::protocol {
namespace {
void ValidateBytes(const RoomConfigurationRequest& request) {
  for (const auto value : request.bytes_at72) {
    if (value > 255) {
      throw std::invalid_argument(
          "Room configuration byte exceeds the native writer range.");
    }
  }
  for (const auto value : request.bytes_at112) {
    if (value > 255) {
      throw std::invalid_argument(
          "Room configuration byte exceeds the native writer range.");
    }
  }
}
}  // namespace

TcpPacket RoomConfigurationRequest::Encode() const {
  ValidateBytes(*this);
  PacketWriter writer;
  writer.WriteUtf16(map_name);
  writer.WriteUtf16(map_directory);
  writer.WriteUtf16(map_key);
  for (const auto value : bytes_at72) writer.WriteUInt32(value);
  for (const auto value : values_at76) writer.WriteUInt32(value);
  for (const auto value : bytes_at112) writer.WriteUInt32(value);
  return {kMessageId, writer.TakeBytes()};
}

RoomConfigurationRequest RoomConfigurationRequest::Decode(
    const TcpPacket& packet) {
  if (packet.message_id != kMessageId) {
    throw std::invalid_argument("Invalid room configuration message.");
  }
  PacketReader reader(packet.payload);
  RoomConfigurationRequest result;
  result.map_name = reader.ReadUtf16();
  result.map_directory = reader.ReadUtf16();
  result.map_key = reader.ReadUtf16();
  for (auto& value : result.bytes_at72) value = reader.ReadUInt32();
  for (auto& value : result.values_at76) value = reader.ReadUInt32();
  for (auto& value : result.bytes_at112) value = reader.ReadUInt32();
  reader.RequireEnd();
  ValidateBytes(result);
  return result;
}
}  // namespace bubble_fighter::server::protocol
