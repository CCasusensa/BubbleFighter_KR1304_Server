#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_BUNDLE_ROWS_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_BUNDLE_ROWS_H_

#include "features/gifts/gift_service.h"
#include "persistence/my_sql_commands.h"

namespace bubble_fighter::server::persistence::gift_sql {
void LoadBundle(MYSQL* connection, features::gifts::GiftRecord& gift);
void InsertBundle(MYSQL* connection, const features::gifts::GiftRecord& gift);
}  // namespace bubble_fighter::server::persistence::gift_sql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_GIFTS_GIFT_BUNDLE_ROWS_H_
