#include <limits>
#include <stdexcept>

#include "protocol/clans/clan_state_report.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
void PlayerSession::CheckClanChannel() {
  CheckClanPlaza();
  if (channel_category_ != 80 || !party_membership_ || connection_handoff_)
    return;
  const auto world = channels_ ? channels_->Find(80) : nullptr;
  const auto now = practice_clock_();
  const bool allowed = party_membership_.CheckAccess([&](auto id, auto epoch,
                                                         auto members) {
    if (!world || !world->clan_access) return false;
    std::vector<features::clans::ClanIdentity> identities;
    for (const auto& member : members)
      identities.push_back(
          {member.account_id, static_cast<std::uint32_t>(member.character_id)});
    const auto revision = world->clan_access->Revision();
    if (channel_poll_at_ && now < *channel_poll_at_ &&
        channel_party_id_ == id && channel_party_epoch_ == epoch &&
        channel_members_ == identities && channel_revision_ == revision)
      return true;
    const auto decision = world->Authorize(members);
    if (decision.result != 0) return false;
    channel_party_id_ = id;
    channel_party_epoch_ = epoch;
    channel_members_ = std::move(identities);
    channel_revision_ = decision.revision;
    channel_poll_at_ = now + std::chrono::seconds{10};
    return true;
  });
  if (!allowed)
    throw std::runtime_error("Clan channel membership is no longer eligible.");
}
void PlayerSession::CheckClanPlaza() {
  if (!party_membership_ || !account_id_ || !plazas_ || connection_handoff_)
    return;
  const auto world = channels_ ? channels_->Find(channel_category_) : nullptr;
  const auto now = practice_clock_();
  const bool allowed = party_membership_.CheckAccess([&](auto id, auto epoch,
                                                         auto members) {
    // Query actual committed residency, including before this connection has
    // adopted a collective result. The party lock serializes group commits.
    const auto current = plazas_->Capture(*account_id_);
    if (!current || !current->clan_id) {
      plaza_clan_poll_at_.reset();
      plaza_clan_id_ = 0;
      return true;
    }
    if (!world || !world->clan_plaza_access) return false;
    std::vector<features::clans::ClanIdentity> identities;
    for (const auto& member : members)
      identities.push_back(
          {member.account_id, static_cast<std::uint32_t>(member.character_id)});
    const auto revision = world->clan_plaza_access->Revision();
    if (plaza_clan_poll_at_ && now < *plaza_clan_poll_at_ &&
        plaza_clan_id_ == current->clan_id && plaza_party_id_ == id &&
        plaza_party_epoch_ == epoch && plaza_clan_members_ == identities &&
        plaza_clan_revision_ == revision)
      return true;
    const auto decision = world->AuthorizePlaza(current->clan_id, members);
    if (!decision.allowed) return false;
    plaza_clan_id_ = current->clan_id;
    plaza_party_id_ = id;
    plaza_party_epoch_ = epoch;
    plaza_clan_members_ = std::move(identities);
    plaza_clan_revision_ = decision.revision;
    plaza_clan_poll_at_ = now + std::chrono::seconds{10};
    return true;
  });
  if (!allowed)
    throw std::runtime_error("Clan plaza membership is no longer eligible.");
}
void PlayerSession::RefreshPartyClan() {
  if (!party_clan_ || !account_id_ || !character_ ||
      state_ == PlayerSessionState::kRejected)
    return;
  const auto now = practice_clock_();
  const auto revision =
      clan_authentication_ ? clan_authentication_->MembershipRevision() : 0;
  if (party_clan_poll_at_ && now < *party_clan_poll_at_ &&
      revision == party_clan_revision_)
    return;
  // Do not hold clan operations while acquiring PartyRegistry. Its snapshot
  // is presentation data, and cannot authorize a later admission or mutation.
  std::unique_lock<std::mutex> operation;
  if (clan_authentication_) {
    operation = clan_authentication_->LockOperations();
    party_clan_revision_ = clan_authentication_->MembershipRevision();
  }
  const auto snapshot = party_clan_(
      {*account_id_, static_cast<std::uint32_t>(character_->character_id)});
  std::uint32_t clan_id = 0;
  if (snapshot && snapshot->profile.membership_status >= 100 &&
      snapshot->profile.membership_status <= 102)
    clan_id = snapshot->profile.clan_id;
  party_clan_id_ = clan_id;
  party_clan_poll_at_ = now + std::chrono::seconds{10};
}
std::vector<protocol::TcpPacket> PlayerSession::PollTrainingClan() {
  if (!training_ || !clan_game_ || !account_id_ || !character_ ||
      training_graduation_sent_)
    return {};
  const auto now = practice_clock_();
  if (training_poll_at_ && now < *training_poll_at_) return {};
  training_poll_at_ = now + std::chrono::seconds{10};
  if (!training_(
          {*account_id_, static_cast<std::uint32_t>(character_->character_id)}))
    return {};
  training_graduation_sent_ = true;
  return {protocol::TrainingClanGraduationNotification{}.Encode()};
}
void PlayerSession::SyncClanScene() {
  if (!clan_authentication_ || !clan_game_) return;
  features::clans::ClanGameScene scene{channel_category_, 0};
  if (state_ != PlayerSessionState::kRejected && !scene_connection_.Pending() &&
      !channel_connection_.Pending() && !connection_handoff_) {
    if (room_ || practice_)
      scene.type = 2;
    else if (state_ == PlayerSessionState::kMenuInitializationSent)
      scene.type = plaza_ ? (plaza_.ClanId() ? 6U : 3U) : 1U;
  }
  clan_authentication_->UpdateScene(clan_game_, scene);
}
void PlayerSession::InvalidateClanScene() {
  if (clan_authentication_ && clan_game_)
    clan_authentication_->UpdateScene(clan_game_, {channel_category_, 0});
}
std::optional<std::vector<protocol::TcpPacket>> PlayerSession::ProcessClan(
    const protocol::TcpPacket& packet) {
  using namespace protocol;
  if (packet.message_id == ClanStateReport::kMessageId ||
      packet.message_id == ClanAppearanceReport::kMessageId) {
    if (packet.message_id == ClanStateReport::kMessageId)
      ClanStateReport::Decode(packet);
    else
      ClanAppearanceReport::Decode(packet);
    if (state_ == PlayerSessionState::kRejected || !clan_authentication_ ||
        !clan_game_ || !account_id_ || !character_ ||
        character_->account_id != *account_id_)
      throw std::runtime_error(
          "Clan state report requires a trusted game character.");
    // Native UI may report an intermediate or stale transition. Membership is
    // owned by the clan database; this observation never changes any authority.
    return std::vector<TcpPacket>{};
  }
  if (packet.message_id == ItemUseRequest::kMessageId) {
    const auto request = ItemUseRequest::Decode(packet);
    if (!features::clans::ClanCreation::SupportsItem(request.item_id))
      return {};
    if (state_ == PlayerSessionState::kRejected || !clan_authentication_ ||
        !clan_game_ || !clan_confirmation_ || !account_id_ || !character_ ||
        character_->account_id != *account_id_ || !character_->character_id ||
        character_->character_id > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error(
          "Clan confirmation requires a trusted game character.");
    // Native S1028 initiates this auxiliary C806. The committed receipt, not
    // the message alone or a cached clan role, authorizes the read-only reply.
    return clan_confirmation_(
        {*account_id_, static_cast<std::uint32_t>(character_->character_id)},
        request, InventoryObservationTime());
  }
  if (packet.message_id != ClanPassportRequest::kMessageId) return {};
  ClanPassportRequest::Decode(packet);
  // The native auxiliary socket can request its passport before C3. Retain
  // only the request; no clan identity is granted until game authentication.
  if (clan_authentication_ &&
      (state_ == PlayerSessionState::kAwaitingTicket ||
       state_ == PlayerSessionState::kAwaitingCharacterCreation ||
       state_ == PlayerSessionState::kAwaitingCharacterProfile)) {
    clan_passport_pending_ = true;
    return std::vector<TcpPacket>{};
  }
  if (state_ == PlayerSessionState::kRejected || !clan_authentication_ ||
      !account_id_ || !character_ || character_->account_id != *account_id_ ||
      !character_->character_id ||
      character_->character_id > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error(
        std::string("Clan Passport requires a trusted game character.") +
        " state=" + std::to_string(static_cast<int>(state_)) +
        " clan=" + std::to_string(static_cast<bool>(clan_authentication_)) +
        " account=" + std::to_string(account_id_.value_or(0)) +
        " character=" +
        std::to_string(character_ ? character_->character_id : 0));
  if (!clan_game_) {
    clan_game_ = clan_authentication_->BindGame(
        {*account_id_, static_cast<std::uint32_t>(character_->character_id)});
  }
  SyncClanScene();
  return std::vector<TcpPacket>{ClanPassportResponse{
      clan_authentication_->Issue(clan_game_, character_->character_name)}
                                    .Encode()};
}
}  // namespace bubble_fighter::server::sessions
