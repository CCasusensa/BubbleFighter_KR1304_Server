#include <chrono>
#include <stdexcept>

#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
std::optional<std::vector<protocol::TcpPacket>> PlayerSession::ProcessGifts(
    const protocol::TcpPacket& packet) {
  using namespace protocol;
  std::optional<features::gifts::GiftRequest> request;
  switch (packet.message_id) {
    case GiftReceivedListRequest::kMessageId:
      request = GiftReceivedListRequest::Decode(packet);
      break;
    case GiftSentListRequest::kMessageId:
      request = GiftSentListRequest::Decode(packet);
      break;
    case GiftSendRequest::kMessageId:
      request = GiftSendRequest::Decode(packet);
      break;
    case GiftReceiveRequest::kMessageId:
      request = GiftReceiveRequest::Decode(packet);
      break;
    case GiftDiscardRequest::kMessageId:
      request = GiftDiscardRequest::Decode(packet);
      break;
    case GiftSendThanksRequest::kMessageId:
      request = GiftSendThanksRequest::Decode(packet);
      break;
    case GiftViewThanksRequest::kMessageId:
      request = GiftViewThanksRequest::Decode(packet);
      break;
    case GiftDeleteSentRequest::kMessageId:
      request = GiftDeleteSentRequest::Decode(packet);
      break;
    case GiftDeleteReceivedRequest::kMessageId:
      request = GiftDeleteReceivedRequest::Decode(packet);
      break;
    default:
      return std::nullopt;
  }
  if (!CommerceInitialized() || !menu_data_ || !gifts_ || !account_id_ ||
      !character_ || !lobby_ || character_->account_id != *account_id_)
    throw std::runtime_error(
        "Gift operation before full authenticated initialization.");
  const auto execute = [&] {
    auto result = gifts_(*character_, *request, InventoryObservationTime());
    if (result.character.account_id != *account_id_ ||
        result.character.character_id != character_->character_id ||
        result.character.character_name != character_->character_name ||
        result.character.character_type != character_->character_type ||
        result.character.color_index != character_->color_index ||
        !result.character.statistics.IsValid())
      throw std::runtime_error("Gift provider changed authenticated identity.");
    character_ = std::move(result.character);
    gift_poll_at_.reset();
    return std::move(result.packets);
  };
  // Inventory grants share the room start/equipment lock. Never grant while
  // playing. Other gift actions cannot change the active match inventory.
  if (std::holds_alternative<GiftReceiveRequest>(*request))
    return ChangeInventory(execute);
  auto packets = execute();
  if (room_) room_.RefreshCharacter(*character_);
  return packets;
}
std::vector<protocol::TcpPacket> PlayerSession::PollGifts() {
  if (!CommerceInitialized() || !gift_count_ || !menu_data_ || !account_id_ ||
      !character_ || !lobby_ || connection_handoff_ ||
      scene_connection_.Pending())
    return {};
  const auto now = practice_clock_();
  if (gift_poll_at_ && now >= *gift_poll_at_ &&
      now - *gift_poll_at_ < std::chrono::seconds(5))
    return {};
  const auto count = gift_count_(*character_, InventoryObservationTime());
  if (count > features::gifts::kGiftCapacity)
    throw std::runtime_error("Invalid gift notification count.");
  gift_poll_at_ = now;
  const bool changed = gift_pending_ ? *gift_pending_ != count : count != 0;
  gift_pending_ = count;
  // First field is opaque; zero is the explicit project baseline.
  if (changed) return {protocol::GiftNewReceiveNotification{0, count}.Encode()};
  return {};
}
}  // namespace bubble_fighter::server::sessions
