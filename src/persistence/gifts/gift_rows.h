#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_ROWS_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_ROWS_H_

#include "features/gifts/gift_service.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence::gift_sql {
std::optional<features::characters::CharacterRecord> FindRecipient(
    MYSQL* connection, std::u16string_view name);
void LockOwners(MYSQL* connection,
                const features::characters::CharacterRecord& owner,
                const features::characters::CharacterRecord& recipient);
std::vector<features::gifts::GiftRecord> LoadVisible(MYSQL* connection,
                                                     std::uint64_t owner,
                                                     std::uint64_t other = 0);
void Insert(MYSQL* connection, const features::gifts::GiftRecord& gift);
// Caller holds the recipient character lock. Nonlocking reads avoid acquiring
// another sender's gift-row locks while holding only this recipient lock.
std::vector<features::gifts::GiftRecord> LoadPending(MYSQL* connection,
                                                     std::uint64_t recipient,
                                                     std::uint64_t now);
void Update(MYSQL* connection, const features::gifts::GiftRecord& gift);
}  // namespace bubble_fighter::server::persistence::gift_sql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_ROWS_H_
