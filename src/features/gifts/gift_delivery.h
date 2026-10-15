#ifndef BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_DELIVERY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_DELIVERY_H_

#include "features/gifts/gift_service.h"
#include "features/inventory/delivery_capacity.h"

namespace bubble_fighter::server::features::gifts {
inventory::PendingDelivery PendingGiftDelivery(
    std::uint64_t recipient, std::span<const GiftRecord> gifts,
    std::uint64_t now);
}  // namespace bubble_fighter::server::features::gifts

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_DELIVERY_H_
