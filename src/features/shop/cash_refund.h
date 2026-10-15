#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_CASH_REFUND_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_CASH_REFUND_H_

#include "features/characters/character_store.h"
#include "features/inventory/cash_inventory.h"
#include "protocol/shop/cash_refund_request.h"
#include "protocol/shop/cash_refund_response.h"

namespace bubble_fighter::server::features::shop {
struct CashRefundResult {
  characters::CharacterRecord character;
  inventory::CashInventory pending;
  protocol::CashRefundResponse response;
  std::uint32_t purchase_id = 0;
  std::uint32_t cash_returned = 0;
  std::vector<std::uint32_t> removed_slot_ids;
  std::vector<protocol::TcpPacket> packets;
};

CashRefundResult PrepareCashRefund(const characters::CharacterRecord& character,
                                   inventory::CashInventory pending,
                                   const protocol::CashRefundRequest& request,
                                   std::uint64_t observed_at_unix_seconds);
}  // namespace bubble_fighter::server::features::shop

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_CASH_REFUND_H_
