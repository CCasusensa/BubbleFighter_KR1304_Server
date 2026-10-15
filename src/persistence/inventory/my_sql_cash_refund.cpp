#include <algorithm>
#include <array>
#include <stdexcept>

#include "persistence/inventory/bundle_rows.h"
#include "persistence/inventory/inventory_rows.h"
#include "persistence/my_sql_commands.h"
#include "persistence/my_sql_inventory_store.h"

namespace bubble_fighter::server::persistence {
features::shop::CashRefundResult MySqlInventoryStore::Refund(
    const features::characters::CharacterRecord& character,
    const protocol::CashRefundRequest& request,
    std::uint64_t observed_at_unix_seconds) {
  using namespace my_sql_commands;
  using namespace inventory_sql;
  auto* connection = connection_.Handle();
  Transaction transaction(connection);
  if (!LockOwner(connection, character.account_id, character.character_id)) {
    throw std::runtime_error("Cash refund owner is unavailable.");
  }
  const auto current = ReadCurrentCharacter(connection, character);
  auto pending =
      LoadCashSlots(connection, character.account_id, character.character_id);
  if (std::any_of(
          pending.bundles.begin(), pending.bundles.end(),
          [&](const auto& row) { return row.slot_id == request.slot_id; })) {
    auto result = features::shop::BundleShop::Refund(
        request, current, std::move(pending), observed_at_unix_seconds);
    if (result.response.result == protocol::CashRefundResponse::kSuccess) {
      PersistBundleRefund(connection, current, result,
                          observed_at_unix_seconds);
      transaction.Commit();
    }
    return result;
  }
  auto result = features::shop::PrepareCashRefund(
      current, std::move(pending), request, observed_at_unix_seconds);
  if (result.response.result != protocol::CashRefundResponse::kSuccess)
    return result;
  // All response encoding has succeeded before the first write.
  Statement receipt(
      connection,
      "UPDATE CashPurchases SET PurchaseState = 2, RefundedAtUnixSeconds = ? WHERE PurchaseId = ? AND CharacterId = ? AND PurchaseState = 0");
  std::array receipt_parameters{Number(observed_at_unix_seconds),
                                Number(result.purchase_id),
                                Number(character.character_id)};
  receipt.Execute(receipt_parameters);
  if (receipt.AffectedRows() != 1)
    throw std::runtime_error("Cash refund receipt changed.");
  if (result.cash_returned != 0) {
    Statement credit(
        connection,
        "UPDATE Characters SET Cash = ? WHERE CharacterId = ? AND AccountId = ? AND Cash = ?");
    std::array parameters{Number(result.character.statistics.cash),
                          Number(character.character_id),
                          Number(character.account_id),
                          Number(current.statistics.cash)};
    credit.Execute(parameters);
    if (credit.AffectedRows() != 1)
      throw std::runtime_error("Cash refund balance changed.");
  }
  for (const auto slot_id : result.removed_slot_ids) {
    Statement remove(
        connection,
        "DELETE FROM CashInventorySlots WHERE SlotId = ? AND CharacterId = ?");
    std::array parameters{Number(slot_id), Number(character.character_id)};
    remove.Execute(parameters);
    if (remove.AffectedRows() != 1)
      throw std::runtime_error("Cash refund slot changed.");
  }
  transaction.Commit();
  return result;
}
}  // namespace bubble_fighter::server::persistence
