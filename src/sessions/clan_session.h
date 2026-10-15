#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_SESSION_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_SESSION_H_

#include "features/clans/clan_creation.h"
#include "features/clans/clan_directory.h"
#include "features/clans/clan_disband.h"
#include "features/clans/clan_master_transfers.h"
#include "features/clans/clan_mutation.h"
#include "features/clans/clan_profile.h"
#include "features/clans/clan_roster.h"
#include "features/clans/training_clan.h"
#include "network/clan_tcp_server.h"

namespace bubble_fighter::server::sessions {
class ClanSession {
 public:
  using Authentication = features::clans::ClanAuthentication;
  ClanSession(std::shared_ptr<Authentication> authentication,
              features::clans::ClanProfileSource profiles,
              Authentication::Now now = Authentication::Clock::now,
              features::clans::ClanCreateHandler create = {},
              features::clans::ClanDirectorySource directory = {},
              features::clans::ClanRosterSource rosters = {},
              features::clans::ClanMutationHandler mutate = {},
              std::shared_ptr<features::clans::ClanMasterTransfers>
                  master_transfers = {},
              features::clans::ClanDisbandHandler disband = {},
              features::clans::TrainingClanHandler training = {});
  network::ClanHandlingResult Process(const protocol::ClanPacket& packet);
  network::ClanHandlingResult Poll();

 private:
  network::ClanHandlingResult Check();
  network::ClanHandlingResult ProcessPacket(const protocol::ClanPacket& packet);
  std::optional<features::clans::ClanSnapshot> Load();
  std::shared_ptr<Authentication> authentication_;
  features::clans::ClanProfileSource profiles_;
  Authentication::Now now_;
  features::clans::ClanCreateHandler create_;
  features::clans::ClanDirectorySource directory_;
  features::clans::ClanRosterSource rosters_;
  features::clans::ClanMutationHandler mutate_;
  std::shared_ptr<features::clans::ClanMasterTransfers> master_transfers_;
  features::clans::ClanDisbandHandler disband_;
  features::clans::TrainingClanHandler training_;
  std::uint64_t delivered_training_ = 0;
  std::uint32_t delivered_disband_ = 0;
  Authentication::Clock::time_point deadline_;
  Authentication::Binding binding_;
  bool authenticated_ = false;
  bool closed_ = false;
};
}  // namespace bubble_fighter::server::sessions

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_SESSION_H_
