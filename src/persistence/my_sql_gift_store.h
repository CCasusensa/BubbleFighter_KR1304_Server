#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_GIFT_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_GIFT_STORE_H_

#include "features/gifts/gift_service.h"
#include "persistence/my_sql_connection.h"

namespace bubble_fighter::server::persistence {
class MySqlGiftStore {
 public:
  explicit MySqlGiftStore(const std::filesystem::path& configuration);
  features::gifts::GiftResult Execute(
      const features::characters::CharacterRecord& owner,
      const features::gifts::GiftRequest& request, std::uint64_t now);
  std::uint32_t Pending(const features::characters::CharacterRecord& owner,
                        std::uint64_t now);

 private:
  features::gifts::GiftResult ExecuteTransaction(
      const features::characters::CharacterRecord& owner,
      const features::gifts::GiftRequest& request, std::uint64_t now);
  MySqlConnection connection_;
};
}  // namespace bubble_fighter::server::persistence

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_GIFT_STORE_H_
