#ifndef BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_SERVICE_H_

#include <optional>
#include <span>
#include <variant>

#include "features/characters/character_store.h"
#include "features/gifts/gift_bundle.h"
#include "features/inventory/cash_inventory.h"
#include "protocol/gifts/gift_messages.h"

namespace bubble_fighter::server::features::gifts {
// Project policies. Native UI pages contain four rows and thanks emotions 2..6.
inline constexpr std::size_t kGiftCapacity = 200;
inline constexpr std::size_t kGiftMessageUnits = 128;
inline constexpr std::size_t kGiftThanksUnits = 24;
inline constexpr std::uint64_t kGiftLifetimeSeconds = 30 * 86400;
inline constexpr std::uint64_t kGiftMaximumTime = 4102444800;
inline constexpr std::uint32_t kGiftMaximumId = 2147483647;

enum class GiftState : std::uint8_t { kPending, kClaimed, kDiscarded };

struct GiftRecord {
  std::uint32_t gift_id = 0;
  std::uint64_t sender_id = 0;
  std::uint64_t recipient_id = 0;
  std::u16string sender_name;
  std::u16string recipient_name;
  std::uint32_t stock_id = 0;
  std::uint32_t item_id = 0;
  inventory::InventoryUsage usage = inventory::InventoryUsage::kCount;
  std::uint32_t amount = 0;
  std::uint32_t max_amount = 0;
  std::uint32_t price = 0;
  bool cash = false;
  std::uint64_t sent_at = 0;
  std::uint64_t handled_at = 0;
  GiftState state = GiftState::kPending;
  bool sender_hidden = false;
  bool recipient_hidden = false;
  std::u16string message;
  std::u16string thanks_message;
  std::uint32_t thanks_emotion = 0;
  std::uint32_t thanks_color = 0;
  protocol::GiftDetails thanks_appearance;
  std::optional<GiftBundle> bundle;
  bool operator==(const GiftRecord&) const = default;
};

using GiftRequest = std::variant<
    protocol::GiftReceivedListRequest, protocol::GiftSentListRequest,
    protocol::GiftSendRequest, protocol::GiftReceiveRequest,
    protocol::GiftDiscardRequest, protocol::GiftSendThanksRequest,
    protocol::GiftViewThanksRequest, protocol::GiftDeleteSentRequest,
    protocol::GiftDeleteReceivedRequest>;

struct GiftResult {
  characters::CharacterRecord character;
  std::vector<protocol::TcpPacket> packets;
  std::optional<GiftRecord> changed;
  std::optional<inventory::CharacterInventory> inventory;
};

void ValidateGift(const GiftRecord& gift);
void ValidateGiftTime(std::uint64_t now);
std::uint32_t PendingGiftCount(std::uint64_t owner,
                               std::span<const GiftRecord> gifts,
                               std::uint64_t now);
GiftResult RefuseGiftSend(const characters::CharacterRecord& sender,
                          std::int32_t result);
GiftResult PrepareGiftSend(
    const protocol::GiftSendRequest& request,
    const characters::CharacterRecord& sender,
    const characters::CharacterRecord& recipient,
    const inventory::CharacterInventory& recipient_inventory,
    const inventory::CashInventory& recipient_cash,
    std::span<const GiftRecord> recipient_gifts,
    std::size_t sender_visible_count, std::uint32_t assigned_id,
    std::uint64_t now);
// Inputs are a complete owner-visible snapshot under its database lock. Hidden
// rows need not be loaded. Grants and replies are prepared before any DB write.
GiftResult PrepareGiftAction(const characters::CharacterRecord& owner,
                             const inventory::CharacterInventory& inventory,
                             std::span<const GiftRecord> gifts,
                             const GiftRequest& request, std::uint64_t now);
}  // namespace bubble_fighter::server::features::gifts

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_SERVICE_H_
