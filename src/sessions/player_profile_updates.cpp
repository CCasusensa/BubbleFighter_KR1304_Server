#include <algorithm>
#include <stdexcept>

#include "features/characters/character_profile.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
std::vector<protocol::TcpPacket> PlayerSession::DrainRoomPackets() {
  auto packets = room_packets_->Drain();
  const auto is_profile = [](const auto& packet) {
    return packet.message_id == protocol::UserStatNotification::kMessageId;
  };
  if (std::none_of(packets.begin(), packets.end(), is_profile)) return packets;
  if (!characters_ || !account_id_ || !character_) {
    throw std::runtime_error(
        "Room profile delivery has no authenticated owner.");
  }
  // A durable receipt is historical. It may already have been committed before
  // a lost response, a retry, or a later purchase. Read once when this
  // connection drains the queue, outside the room lock. Never use receipt
  // balances as the current profile, and never modify its reward deltas or
  // replay identity.
  auto current = characters_->FindByAccount(*account_id_);
  if (!current || current->account_id != *account_id_ ||
      current->character_id != character_->character_id ||
      current->character_name != character_->character_name ||
      current->character_type != character_->character_type ||
      current->color_index != character_->color_index) {
    throw std::runtime_error("Room profile delivery owner changed.");
  }
  for (auto& packet : packets) {
    if (!is_profile(packet)) continue;
    packet = features::characters::CharacterProfile::BindKnownFields(
                 protocol::UserStatNotification::Decode(packet), *current,
                 *account_id_)
                 .Encode();
  }
  // All encoding succeeds before either cache is published. The application
  // binds XP thresholds using the current XP after this step. Unknown envelope
  // fields and all non-S105 packets, including the historical S614, are
  // retained.
  if (room_) room_.RefreshCharacter(*current);
  character_ = std::move(current);
  return packets;
}
}  // namespace bubble_fighter::server::sessions
