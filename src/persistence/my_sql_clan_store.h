#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_CLAN_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_CLAN_STORE_H_

#include "features/clans/clan_channel_access.h"
#include "features/clans/clan_creation.h"
#include "features/clans/clan_directory.h"
#include "features/clans/clan_disband.h"
#include "features/clans/clan_master_transfers.h"
#include "features/clans/clan_mutation.h"
#include "features/clans/clan_plaza_access.h"
#include "features/clans/clan_profile.h"
#include "features/clans/clan_roster.h"
#include "features/clans/training_clan.h"
#include "persistence/my_sql_connection.h"

namespace bubble_fighter::server::persistence {
class MySqlClanStore {
 public:
  explicit MySqlClanStore(const std::filesystem::path& configuration);
  std::optional<features::clans::ClanSnapshot> Load(
      const features::clans::ClanIdentity& identity);
  std::optional<std::vector<features::clans::ClanChannelMember>>
  ReadChannelMembers(std::span<const features::clans::ClanIdentity> identities);
  std::optional<std::vector<features::clans::ClanPlazaMember>> ReadPlazaMembers(
      std::span<const features::clans::ClanIdentity> identities);
  std::optional<protocol::ClanDirectoryResponse> Search(
      const features::clans::ClanIdentity& identity,
      const protocol::ClanDirectoryRequest& request);
  std::optional<std::vector<protocol::ClanPacket>> ReadRoster(
      const features::clans::ClanIdentity& identity,
      features::clans::ClanRosterKind kind);
  features::clans::ClanMutationResult Mutate(
      const features::clans::ClanIdentity& identity,
      const features::clans::ClanMutation& request,
      const features::clans::ClanMutationAuthority& authorized);
  std::optional<features::clans::ClanMasterTransferParties>
  PrepareMasterTransfer(const features::clans::ClanIdentity& requester,
                        std::uint32_t target);
  features::clans::ClanMutationResult CommitMasterTransfer(
      const features::clans::ClanMasterTransferParties& parties,
      const features::clans::ClanMutationAuthority& authorized);
  features::clans::ClanMutationResult Disband(
      const features::clans::ClanIdentity& identity,
      const protocol::ClanDisbandRequest& request, std::uint64_t now,
      std::uint64_t maintenance,
      const features::clans::ClanMutationAuthority& authorized);
  std::uint64_t DisbandDue(std::uint64_t now);
  void LockService();
  features::clans::TrainingClanResult Training(
      const features::clans::ClanIdentity& identity,
      features::clans::TrainingClanAction action, std::uint64_t now,
      const features::clans::ClanMutationAuthority& authorized);
  protocol::ClanCreateResponse Create(
      const features::clans::ClanIdentity& identity,
      const protocol::ClanCreateRequest& request,
      std::uint64_t observed_at_unix_seconds,
      const features::clans::ClanCreationAuthority& authorized);
  std::vector<protocol::TcpPacket> ConfirmCreation(
      const features::clans::ClanIdentity& identity,
      const protocol::ItemUseRequest& request,
      std::uint64_t observed_at_unix_seconds);

 private:
  std::optional<features::clans::ClanMasterTransferParties> ReadMasterTransfer(
      const features::clans::ClanIdentity& requester, std::uint32_t target);
  std::optional<features::clans::ClanSnapshot> LoadInTransaction(
      const features::clans::ClanIdentity& identity);
  std::optional<std::vector<protocol::ClanPacket>> ReadRosterInTransaction(
      const features::clans::ClanIdentity& identity,
      features::clans::ClanRosterKind kind);
  MySqlConnection connection_;
};
}  // namespace bubble_fighter::server::persistence

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MY_SQL_CLAN_STORE_H_
