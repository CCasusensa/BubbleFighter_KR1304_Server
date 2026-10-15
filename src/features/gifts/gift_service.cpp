#include "features/gifts/gift_service.h"

#include <algorithm>
#include <array>
#include <bit>
#include <set>
#include <stdexcept>
#include <type_traits>

#include "features/characters/character_creation.h"
#include "features/characters/character_profile.h"
#include "features/chat/scene_chat.h"
#include "features/gifts/generated/gift_catalog_data.h"
#include "features/gifts/gift_delivery.h"
#include "features/inventory/inventory_grant.h"
#include "features/inventory/inventory_presentation.h"
#include "features/inventory/inventory_validation.h"
#include "features/inventory/item_catalog.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/shop_stock_catalog.h"
#include "features/shop/stock_set_catalog.h"
#include "protocol/inventory/inventory_view_update_notification.h"
#include "protocol/shop/cash_balance_response.h"

namespace bubble_fighter::server::features::gifts {
namespace {
using namespace protocol;
using inventory::InventoryGrant;
using inventory::InventoryGrantStatus;
using inventory::InventoryUsage;

InventoryGrantStatus GrantItem(inventory::CharacterInventory& inventory,
                               std::uint32_t item_id, std::uint32_t amount,
                               std::uint32_t max_amount, std::uint64_t now) {
  if (inventory.slots.size() >= InventoryItemRecord::kMaxCollectionEntries &&
      std::none_of(inventory.slots.begin(), inventory.slots.end(),
                   [&](const auto& slot) { return slot.item_id == item_id; }))
    return InventoryGrantStatus::kLimitExceeded;
  return InventoryGrant::Apply(inventory, item_id, amount, max_amount, now);
}

bool Text(std::u16string_view text, std::size_t maximum) {
  return text.empty() || chat::SceneChat::ValidText(text, maximum);
}
void Owner(const characters::CharacterRecord& owner) {
  if (!inventory::IsValidInventoryCharacter(owner) ||
      owner.character_id > kGiftMaximumId)
    throw std::invalid_argument("Invalid trusted gift owner.");
}
std::uint16_t State(const GiftRecord& gift, std::uint64_t now) {
  if (gift.state != GiftState::kPending)
    return static_cast<std::uint16_t>(gift.state);
  return now >= gift.sent_at + kGiftLifetimeSeconds ? 3 : 0;
}
std::uint64_t Date(std::uint64_t seconds) {
  // E45A20/E46A70 use VariantTimeToSystemTime. UTC mapping is project policy.
  return std::bit_cast<std::uint64_t>(25569.0 +
                                      static_cast<double>(seconds) / 86400.0);
}
GiftDetails Appearance(const characters::CharacterRecord& owner,
                       const inventory::CharacterInventory& inventory,
                       std::uint64_t now) {
  const auto view = inventory::CreateInventoryEquipment(owner, inventory, now);
  GiftDetails result;
  result.value_at0 = view.value;
  result.values_at4 = view.groups[0];
  result.values_at16 = view.groups[1];
  result.values_at28 = view.groups[2];
  result.bytes_at40 = view.bytes;
  for (const auto& row : view.details)
    result.records_at52.push_back(
        {row.values[0], row.values[1], row.entries, row.final_value, row.text});
  return result;
}
void Snapshot(const characters::CharacterRecord& owner,
              const inventory::CharacterInventory& inventory,
              std::span<const GiftRecord> gifts, std::uint64_t now) {
  Owner(owner);
  ValidateGiftTime(now);
  if (!inventory::BelongsToCharacter(owner, inventory) ||
      !inventory::MatchesItemCatalog(inventory) ||
      gifts.size() > 2 * kGiftCapacity)
    throw std::invalid_argument("Invalid trusted gift snapshot.");
  (void)inventory.ToRecords(owner.account_id, now);
  std::set<std::uint32_t> ids;
  std::size_t sent = 0, received = 0;
  for (const auto& gift : gifts) {
    ValidateGift(gift);
    if (gift.sent_at > now || gift.handled_at > now ||
        !ids.insert(gift.gift_id).second ||
        (gift.sender_id != owner.character_id &&
         gift.recipient_id != owner.character_id))
      throw std::invalid_argument("Invalid trusted gift snapshot.");
    sent += gift.sender_id == owner.character_id && !gift.sender_hidden;
    received +=
        gift.recipient_id == owner.character_id && !gift.recipient_hidden;
  }
  if (sent > kGiftCapacity || received > kGiftCapacity)
    throw std::invalid_argument("Invalid trusted gift snapshot.");
}
std::vector<const GiftRecord*> Visible(std::uint64_t owner,
                                       std::span<const GiftRecord> gifts,
                                       bool sent) {
  std::vector<const GiftRecord*> rows;
  for (const auto& gift : gifts)
    if (sent ? gift.sender_id == owner && !gift.sender_hidden
             : gift.recipient_id == owner && !gift.recipient_hidden)
      rows.push_back(&gift);
  std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
    return a->gift_id > b->gift_id;
  });
  return rows;
}
}  // namespace

void ValidateGiftTime(std::uint64_t now) {
  if (now == 0 || now > kGiftMaximumTime)
    throw std::invalid_argument("Invalid gift clock.");
}
void ValidateGift(const GiftRecord& gift) {
  const auto* item = inventory::ItemCatalog::Find(gift.item_id);
  const auto* stock = shop::ShopStockCatalog::Find(gift.stock_id);
  if (gift.gift_id == 0 || gift.gift_id > kGiftMaximumId ||
      gift.sender_id == 0 || gift.sender_id > kGiftMaximumId ||
      gift.recipient_id == 0 || gift.recipient_id > kGiftMaximumId ||
      gift.sender_id == gift.recipient_id ||
      characters::CharacterCreation::ValidateName(gift.sender_name) != 0 ||
      characters::CharacterCreation::ValidateName(gift.recipient_name) != 0 ||
      gift.price > characters::CharacterStatistics::kMaxValue ||
      gift.sent_at == 0 ||
      gift.sent_at > kGiftMaximumTime - kGiftLifetimeSeconds ||
      gift.state > GiftState::kDiscarded ||
      (gift.state == GiftState::kPending
           ? gift.handled_at != 0
           : gift.handled_at < gift.sent_at ||
                 gift.handled_at > kGiftMaximumTime) ||
      !Text(gift.message, kGiftMessageUnits) ||
      !Text(gift.thanks_message, kGiftThanksUnits) ||
      (gift.thanks_emotion != 0 &&
       (gift.state != GiftState::kClaimed || gift.thanks_emotion < 2 ||
        gift.thanks_emotion > 6)) ||
      (gift.thanks_emotion == 0 &&
       (!gift.thanks_message.empty() || gift.thanks_color != 0 ||
        gift.thanks_appearance != GiftDetails{})))
    throw std::invalid_argument("Invalid persisted gift record.");
  if (gift.bundle) {
    if (gift.item_id != gift.stock_id || gift.usage != InventoryUsage::kCount ||
        gift.amount != 1 || gift.max_amount != 1)
      throw std::invalid_argument("Invalid gift bundle group terms.");
    ValidateGiftBundle(gift.stock_id, *gift.bundle);
  } else if (!item || item->usage != gift.usage || !stock ||
             stock->item_id != gift.item_id || gift.amount == 0 ||
             gift.max_amount == 0 ||
             (gift.usage != InventoryUsage::kCount &&
              gift.usage != InventoryUsage::kDuration &&
              gift.usage != InventoryUsage::kLife)) {
    throw std::invalid_argument("Invalid persisted gift item terms.");
  }
  if (gift.thanks_emotion != 0) {
    if (gift.thanks_appearance.value_at0 < 300000 ||
        !characters::CharacterCreation::SupportsSelection(
            gift.thanks_appearance.value_at0 - 300000, gift.thanks_color))
      throw std::invalid_argument("Invalid persisted gift appearance.");
    (void)GiftViewThanksResponse{1,
                                 gift.gift_id,
                                 gift.recipient_name,
                                 gift.thanks_message,
                                 gift.thanks_emotion,
                                 gift.thanks_color,
                                 gift.thanks_appearance}
        .Encode();
  }
}
std::uint32_t PendingGiftCount(std::uint64_t owner,
                               std::span<const GiftRecord> gifts,
                               std::uint64_t now) {
  ValidateGiftTime(now);
  std::uint32_t result = 0;
  for (const auto& gift : gifts) {
    ValidateGift(gift);
    if (gift.recipient_id == owner && !gift.recipient_hidden &&
        gift.sent_at <= now && State(gift, now) == 0)
      ++result;
  }
  return result;
}
GiftResult RefuseGiftSend(const characters::CharacterRecord& sender,
                          std::int32_t result) {
  Owner(sender);
  if (result == 1) throw std::invalid_argument("Invalid gift refusal.");
  return {sender,
          {GiftSendResponse{{}, 0, static_cast<std::uint32_t>(result), {}, {}}
               .Encode()},
          {},
          {}};
}

GiftResult PrepareGiftSend(
    const GiftSendRequest& request, const characters::CharacterRecord& sender,
    const characters::CharacterRecord& recipient,
    const inventory::CharacterInventory& recipient_inventory,
    const inventory::CashInventory& recipient_cash,
    std::span<const GiftRecord> recipient_gifts,
    std::size_t sender_visible_count, std::uint32_t assigned_id,
    std::uint64_t now) {
  Owner(sender);
  Snapshot(recipient, recipient_inventory, recipient_gifts, now);
  const auto refuse = [&](std::int32_t code) {
    return RefuseGiftSend(sender, code);
  };
  if (sender.character_id == recipient.character_id) return refuse(-18);
  if (request.recipient_name != recipient.character_name) return refuse(-1);
  if (!request.text_at24.empty() || !Text(request.message, kGiftMessageUnits))
    return refuse(-76);
  if (sender_visible_count >= kGiftCapacity ||
      Visible(recipient.character_id, recipient_gifts, false).size() >=
          kGiftCapacity)
    return refuse(8);
  if (now > kGiftMaximumTime - kGiftLifetimeSeconds) return refuse(-15);
  GiftRecord gift;
  gift.gift_id = assigned_id;
  gift.sender_id = sender.character_id;
  gift.recipient_id = recipient.character_id;
  gift.sender_name = sender.character_name;
  gift.recipient_name = recipient.character_name;
  gift.sent_at = now;
  gift.message = request.message;
  if (const auto* set = shop::StockSetCatalog::Find(request.stock_id)) {
    gift.bundle = PrepareGiftBundle(request.stock_id);
    if (!gift.bundle || recipient.statistics.level < set->required_level)
      return refuse(-15);
    gift.stock_id = gift.item_id = request.stock_id;
    gift.usage = InventoryUsage::kCount;
    gift.amount = gift.max_amount = 1;
    gift.price = static_cast<std::uint32_t>(set->sell_price);
    gift.cash = set->currency != shop::ShopCurrency::kLucci;
  } else {
    const auto* stock = shop::ShopStockCatalog::Find(request.stock_id);
    const auto* offer =
        stock ? shop::ShopCatalog::FindItem(stock->item_id) : nullptr;
    const auto* item =
        stock ? inventory::ItemCatalog::Find(stock->item_id) : nullptr;
    if (!stock || !offer || !item ||
        !std::binary_search(kGiftableItemIds.begin(), kGiftableItemIds.end(),
                            stock->item_id) ||
        stock->duration == 0 ||
        stock->sell_price > characters::CharacterStatistics::kMaxValue ||
        (item->usage != InventoryUsage::kCount &&
         item->usage != InventoryUsage::kLife &&
         item->usage != InventoryUsage::kDuration))
      return refuse(-15);
    gift.stock_id = stock->stock_id;
    gift.item_id = stock->item_id;
    gift.usage = item->usage;
    gift.amount = stock->duration;
    gift.max_amount = item->usage == InventoryUsage::kCount
                          ? (std::max)(offer->max_amount, stock->duration)
                          : offer->max_amount;
    gift.price = stock->sell_price;
    gift.cash = offer->currency != shop::ShopCurrency::kLucci;
  }
  if (recipient_cash.account_id != recipient.account_id ||
      recipient_cash.character_id != recipient.character_id)
    throw std::invalid_argument("Gift cash reservation owner mismatch.");
  (void)recipient_cash.CreateNotification(recipient.account_id, now).Encode();
  auto pending =
      PendingGiftDelivery(recipient.character_id, recipient_gifts, now);
  inventory::AppendCashDelivery(pending, recipient_cash);
  if (inventory::CheckDeliveryCapacity(recipient, recipient_inventory, pending,
                                       now) != InventoryGrantStatus::kGranted)
    return refuse(-17);
  ValidateGift(gift);
  const std::array candidate{gift};
  auto addition = PendingGiftDelivery(recipient.character_id, candidate, now);
  pending.components.insert(pending.components.end(),
                            addition.components.begin(),
                            addition.components.end());
  pending.bonus_lucci += addition.bonus_lucci;
  if (pending.bonus_lucci >
      characters::CharacterStatistics::kMaxValue - recipient.statistics.lucci)
    return refuse(-17);
  const auto status = inventory::CheckDeliveryCapacity(
      recipient, recipient_inventory, pending, now);
  if (status == InventoryGrantStatus::kAlreadyOwned) return refuse(-17);
  if (status != InventoryGrantStatus::kGranted) return refuse(-15);
  const bool cash = gift.cash;
  const auto balance = cash ? sender.statistics.cash : sender.statistics.lucci;
  if (balance < gift.price) return refuse(cash ? 27 : -3);
  if (assigned_id == 0 || assigned_id > kGiftMaximumId)
    throw std::invalid_argument("Gift allocation exhausted.");
  ValidateGift(gift);
  GiftResult result{sender, {}, gift, {}};
  (cash ? result.character.statistics.cash
        : result.character.statistics.lucci) -= gift.price;
  result.packets.push_back(characters::CharacterProfile::BindKnownFields(
                               characters::CreateCharacterProfileBaseline(),
                               result.character, sender.account_id)
                               .Encode());
  if (cash)
    result.packets.push_back(
        CashBalanceResponse{0, result.character.statistics.cash, 0}.Encode());
  // The ordinary success dialog consumes result. The other fields also feed
  // conditional platform callbacks in 9B9A40; retain an explicit empty/zero
  // project baseline until that external integration is implemented.
  result.packets.push_back(GiftSendResponse{{}, 0, 1, {}, {}}.Encode());
  return result;
}

GiftResult PrepareGiftAction(const characters::CharacterRecord& owner,
                             const inventory::CharacterInventory& inventory,
                             std::span<const GiftRecord> gifts,
                             const GiftRequest& request, std::uint64_t now) {
  Snapshot(owner, inventory, gifts, now);
  GiftResult result{owner, {}, {}, {}};
  const auto count = PendingGiftCount(owner.character_id, gifts, now);
  std::visit(
      [&](const auto& action) {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, GiftSendRequest>) {
          throw std::invalid_argument("Gift send needs both trusted owners.");
        } else if constexpr (std::is_same_v<T, GiftReceivedListRequest> ||
                             std::is_same_v<T, GiftSentListRequest>) {
          constexpr bool sent = std::is_same_v<T, GiftSentListRequest>;
          const auto rows = Visible(owner.character_id, gifts, sent);
          const bool valid = action.count > 0 && action.count <= 4;
          using Response = std::conditional_t<sent, GiftSentListResponse,
                                              GiftReceivedListResponse>;
          Response response;
          response.result = valid ? 1 : 2;
          response.total_count = static_cast<std::uint32_t>(rows.size());
          if constexpr (!sent) response.value_at8 = count;
          for (std::size_t i = action.offset;
               valid && i < rows.size() &&
               response.records.size() < action.count;
               ++i) {
            const auto& row = *rows[i];
            if constexpr (sent)
              response.records.push_back(
                  {row.gift_id, Date(row.sent_at), 0, 0, row.recipient_name,
                   row.stock_id, State(row, now),
                   static_cast<std::uint16_t>(
                       row.thanks_emotion ? row.thanks_emotion : 1),
                   row.message, row.thanks_message});
            else
              response.records.push_back(
                  {row.gift_id, Date(row.sent_at),
                   static_cast<std::uint32_t>(row.sender_id), row.sender_name,
                   row.stock_id, State(row, now), row.message,
                   Date(row.sent_at + kGiftLifetimeSeconds)});
          }
          result.packets.push_back(response.Encode());
        } else {
          constexpr bool sender = std::is_same_v<T, GiftDeleteSentRequest> ||
                                  std::is_same_v<T, GiftViewThanksRequest>;
          const auto found =
              std::find_if(gifts.begin(), gifts.end(), [&, sender = sender](const auto& row) {
                return row.gift_id == action.gift_id &&
                       (sender ? row.sender_id == owner.character_id &&
                                     !row.sender_hidden
                               : row.recipient_id == owner.character_id &&
                                     !row.recipient_hidden);
              });
          const bool exists = found != gifts.end();
          if constexpr (std::is_same_v<T, GiftReceiveRequest>) {
            GiftReceiveResponse response{0xffffffffU, action.gift_id, 0, count};
            if (exists && State(*found, now) == 0) {
              auto granted = inventory;
              const auto bonus = found->bundle ? found->bundle->bonus_lucci : 0;
              const bool fits_balance =
                  bonus <= characters::CharacterStatistics::kMaxValue -
                               owner.statistics.lucci;
              const auto status =
                  found->bundle
                      ? GrantGiftBundle(granted, *found->bundle, now)
                      : GrantItem(granted, found->item_id, found->amount,
                                  found->max_amount, now);
              if (fits_balance && status == InventoryGrantStatus::kGranted) {
                auto changed = *found;
                changed.state = GiftState::kClaimed;
                changed.handled_at = now;
                const auto view =
                    inventory::CreateInventoryEquipment(owner, granted, now);
                result.packets = {
                    granted.CreateNotification(owner.account_id, now).Encode(),
                    InventoryViewUpdateNotification{view}.Encode()};
                if (bonus != 0) {
                  result.character.statistics.lucci += bonus;
                  result.packets.insert(
                      result.packets.begin(),
                      characters::CharacterProfile::BindKnownFields(
                          characters::CreateCharacterProfileBaseline(),
                          result.character, owner.account_id)
                          .Encode());
                }
                response = {0, action.gift_id, found->stock_id, count - 1};
                result.changed = std::move(changed);
                result.inventory = std::move(granted);
              }
            }
            result.packets.push_back(response.Encode());
          } else if constexpr (std::is_same_v<T, GiftDiscardRequest>) {
            const bool allowed = exists && State(*found, now) == 0;
            if (allowed) {
              result.changed = *found;
              result.changed->state = GiftState::kDiscarded;
              result.changed->handled_at = now;
            }
            result.packets.push_back(GiftDiscardResponse{
                allowed ? 1U : 2U, action.gift_id, count - (allowed ? 1 : 0)}
                                         .Encode());
          } else if constexpr (std::is_same_v<T, GiftSendThanksRequest>) {
            const bool allowed =
                exists && found->state == GiftState::kClaimed &&
                found->thanks_emotion == 0 && action.value_at28 >= 2 &&
                action.value_at28 <= 6 &&
                Text(action.message, kGiftThanksUnits);
            if (allowed) {
              result.changed = *found;
              result.changed->thanks_message = action.message;
              result.changed->thanks_emotion = action.value_at28;
              result.changed->thanks_color = owner.color_index;
              result.changed->thanks_appearance =
                  Appearance(owner, inventory, now);
            }
            result.packets.push_back(
                GiftSendThanksResponse{allowed ? 1U : 2U}.Encode());
          } else if constexpr (std::is_same_v<T, GiftViewThanksRequest>) {
            GiftViewThanksResponse response;
            if (exists && found->thanks_emotion != 0)
              response = {1,
                          found->gift_id,
                          found->recipient_name,
                          found->thanks_message,
                          found->thanks_emotion,
                          found->thanks_color,
                          found->thanks_appearance};
            result.packets.push_back(response.Encode());
          } else {
            const bool allowed = exists && (sender || State(*found, now) != 0);
            if (allowed) {
              result.changed = *found;
              if constexpr (sender)
                result.changed->sender_hidden = true;
              else
                result.changed->recipient_hidden = true;
            }
            if constexpr (sender)
              result.packets.push_back(
                  GiftDeleteSentResponse{allowed ? 1U : 2U, action.gift_id}
                      .Encode());
            else
              result.packets.push_back(GiftDeleteReceivedResponse{
                  allowed ? 1U : 2U, action.gift_id, count}
                                           .Encode());
          }
        }
      },
      request);
  if (result.changed) ValidateGift(*result.changed);
  return result;
}
}  // namespace bubble_fighter::server::features::gifts
