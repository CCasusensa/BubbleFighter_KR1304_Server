#include "features/gifts/gift_delivery.h"

namespace bubble_fighter::server::features::gifts {
inventory::PendingDelivery PendingGiftDelivery(
    std::uint64_t recipient, std::span<const GiftRecord> gifts,
    std::uint64_t now) {
  ValidateGiftTime(now);
  inventory::PendingDelivery result;
  for (const auto& gift : gifts) {
    ValidateGift(gift);
    if (gift.recipient_id != recipient || gift.recipient_hidden ||
        gift.state != GiftState::kPending ||
        now >= gift.sent_at + kGiftLifetimeSeconds)
      continue;
    if (gift.bundle) {
      result.components.insert(result.components.end(),
                               gift.bundle->components.begin(),
                               gift.bundle->components.end());
      result.bonus_lucci += gift.bundle->bonus_lucci;
    } else {
      result.components.push_back({gift.stock_id, gift.item_id, gift.usage,
                                   gift.amount, gift.max_amount});
    }
  }
  return result;
}
}  // namespace bubble_fighter::server::features::gifts
