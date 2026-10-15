#include "features/shop/cash_refund.h"

#include <algorithm>
#include <stdexcept>

#include "features/characters/character_profile.h"
#include "features/inventory/inventory_validation.h"
#include "protocol/shop/cash_balance_response.h"

namespace bubble_fighter::server::features::shop {
CashRefundResult PrepareCashRefund(const characters::CharacterRecord& character,
                                   inventory::CashInventory pending,
                                   const protocol::CashRefundRequest& request,
                                   std::uint64_t observed_at_unix_seconds) {
  using Response = protocol::CashRefundResponse;
  (void)request.Encode();
  if (!inventory::IsValidInventoryCharacter(character) ||
      character.account_id != pending.account_id ||
      character.character_id != pending.character_id) {
    throw std::invalid_argument("Cash refund ownership mismatch.");
  }
  (void)pending.CreateNotification(character.account_id).Encode();
  CashRefundResult result{
      character, std::move(pending), {request.slot_id, Response::kFailed, {}}};
  const auto finish = [&] {
    result.packets.push_back(result.response.Encode());
    result.packets.push_back(
        result.pending
            .CreateNotification(character.account_id, observed_at_unix_seconds)
            .Encode());
    return result;
  };
  const auto slot = std::find_if(
      result.pending.slots.begin(), result.pending.slots.end(),
      [&](const auto& value) { return value.slot_id == request.slot_id; });
  if (!request.text_at0.empty() || slot == result.pending.slots.end() ||
      slot->purchase_id == 0) {
    return finish();
  }
  const auto purchase =
      std::find_if(result.pending.purchases.begin(),
                   result.pending.purchases.end(), [&](const auto& value) {
                     return value.purchase_id == slot->purchase_id;
                   });
  if (purchase == result.pending.purchases.end() || purchase->claimed)
    return finish();
  if (observed_at_unix_seconds >= purchase->purchased_at_unix_seconds &&
      observed_at_unix_seconds - purchase->purchased_at_unix_seconds >=
          inventory::kCashRefundWindowSeconds) {
    result.response.result = Response::kExpired;
    return finish();
  }
  if (!inventory::CanRefundCashPurchase(*purchase, observed_at_unix_seconds) ||
      purchase->cash_price > characters::CharacterStatistics::kMaxValue -
                                 character.statistics.cash) {
    return finish();
  }
  result.purchase_id = purchase->purchase_id;
  result.cash_returned = purchase->cash_price;
  result.character.statistics.cash += result.cash_returned;
  for (const auto& value : result.pending.slots) {
    if (value.purchase_id == result.purchase_id)
      result.removed_slot_ids.push_back(value.slot_id);
  }
  std::erase_if(result.pending.slots, [&](const auto& value) {
    return value.purchase_id == result.purchase_id;
  });
  result.pending.purchases.erase(purchase);
  result.response.result = Response::kSuccess;
  // S824 needs the old slot; the complete S810 replacement follows it.
  result.packets = {
      characters::CharacterProfile::BindKnownFields(
          characters::CreateCharacterProfileBaseline(), result.character,
          character.account_id)
          .Encode(),
      protocol::CashBalanceResponse{0, result.character.statistics.cash, 0}
          .Encode()};
  return finish();
}
}  // namespace bubble_fighter::server::features::shop
