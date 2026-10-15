#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MASTER_TRANSFERS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MASTER_TRANSFERS_H_

#include <map>
#include <mutex>
#include <variant>

#include "features/clans/clan_mutation.h"
#include "features/clans/clan_profile.h"
#include "protocol/clans/clan_master_transfer_messages.h"

namespace bubble_fighter::server::features::clans {
struct ClanMasterTransferParties {
  ClanIdentity requester;
  ClanIdentity target;
  std::uint32_t clan_id = 0;
  std::u16string requester_name;
  std::u16string target_name;
  std::uint32_t target_status = 0;
  bool operator==(const ClanMasterTransferParties&) const = default;
};
using ClanMasterTransferSource =
    std::function<std::optional<ClanMasterTransferParties>(const ClanIdentity&,
                                                           std::uint32_t)>;
using ClanMasterTransferCommitter = std::function<ClanMutationResult(
    const ClanMasterTransferParties&, const ClanMutationAuthority&)>;
using ClanMasterTransferAction =
    std::variant<protocol::ClanMasterTransferRequest,
                 protocol::ClanMasterTransferAnswerRequest,
                 protocol::ClanMasterTransferCommitRequest>;

// Native consent has three phases. A successful answer alone never changes
// membership. Pending grants are bound to both connection and scene
// generations.
class ClanMasterTransfers {
 public:
  using Authentication = ClanAuthentication;
  ClanMasterTransfers(std::shared_ptr<Authentication> authentication,
                      ClanMasterTransferSource source,
                      ClanMasterTransferCommitter commit,
                      ClanProfileSource profiles,
                      Authentication::Now now = Authentication::Clock::now);
  ClanMutationResult Process(const Authentication::Binding& binding,
                             const ClanMasterTransferAction& action);

 private:
  struct Pending {
    ClanMasterTransferParties parties;
    Authentication::Binding requester;
    Authentication::Binding target;
    std::uint64_t requester_epoch = 0;
    std::uint64_t target_epoch = 0;
    Authentication::Clock::time_point deadline;
    bool accepted = false;
  };
  bool Current(const Pending& pending) const;
  ClanMutationResult RefuseCommit(const Authentication::Binding& binding);
  std::shared_ptr<Authentication> authentication_;
  ClanMasterTransferSource source_;
  ClanMasterTransferCommitter commit_;
  ClanProfileSource profiles_;
  Authentication::Now now_;
  std::mutex mutex_;
  std::map<std::uint32_t, Pending> pending_;
  std::uint64_t next_key_ = 1;
};
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_MASTER_TRANSFERS_H_
