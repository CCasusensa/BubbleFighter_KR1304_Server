#include <array>
#include <utility>

#include "features/scenes/scene_roster.h"
#include "features/social/room_invitations.h"

namespace bubble_fighter::server::features::social {
std::optional<RoomInvitations::Destination> RoomInvitations::CaptureTarget(
    const Member& owner) const {
  if (owner.transitioning) return {};
  const auto room = rooms_->InvitationTarget(owner.character.character_id);
  if (room) {
    if (room->inviter_name != owner.character.character_name) return {};
    return Destination{*room, {}, {}};
  }
  if (!owner.available || !parties_ || !plazas_) return {};
  const auto party = parties_->Capture(owner.character.character_name);
  if (!party) return {};
  return Destination{{}, party, plazas_->Capture(owner.character.account_id)};
}

std::uint32_t RoomInvitations::CheckTarget(const Destination& target,
                                           const Member& owner) const {
  if (owner.transitioning) return 21;
  if (target.party) {
    if (!owner.available || !parties_ || !plazas_ ||
        rooms_->FindByCharacter(owner.character.character_id))
      return 21;
    const auto current = parties_->Capture(owner.character.character_name);
    if (!current || current->party_id != target.party->party_id ||
        current->epoch != target.party->epoch ||
        current->member_token != target.party->member_token ||
        current->member_name != target.party->member_name)
      return 21;
    const auto plaza = plazas_->Capture(owner.character.account_id);
    if (plaza.has_value() != target.plaza.has_value()) return 21;
    if (plaza && (plaza->scene != target.plaza->scene ||
                  plaza->account != target.plaza->account ||
                  plaza->token != target.plaza->token ||
                  plaza->clan_id != target.plaza->clan_id))
      return 21;
    return 0;
  }
  const auto room = rooms_->Find(target.room.room_id);
  if (!room) return 21;
  if (room->state != rooms::RoomState::kWaiting) return 8;
  const auto current = rooms_->InvitationTarget(owner.character.character_id);
  if (!current || current->room_id != target.room.room_id ||
      current->epoch != target.room.epoch ||
      current->inviter_account_id != target.room.inviter_account_id ||
      current->inviter_name != owner.character.character_name)
    return 21;
  if (room->occupants.size() >= room->slot_count) return 15;
  return 0;
}

InvitationAdmission RoomInvitations::AcceptParty(
    const Destination& target, rooms::RoomOccupant joiner,
    const PartyAdmissionInput& input) {
  InvitationAdmission result;
  const auto fail = [&](std::uint32_t reason) {
    result.responses.push_back(protocol::MoveToInviterFailedNotification{
        target.party->member_name, reason}
                                   .Encode());
  };
  if (!input.membership || !*input.membership || !input.scene_sink ||
      !parties_->Owns(*input.membership) ||
      input.membership->Character() != joiner.character ||
      joiner.equipment.key != joiner.character.character_id) {
    fail(21);
    return result;
  }
  scenes::SceneSlot player{std::move(joiner.member), joiner.character,
                           std::move(joiner.equipment)};
  clans::ClanPlazaScope scope;
  auto authority = input.authority;
  if (target.plaza && target.plaza->clan_id) {
    // This authority checks the full candidate group, including the target
    // party, in the configured channel. Do not nest the category80 lock.
    authority = [&](auto members, const auto& commit) {
      return clan_plaza_authority_ &&
             clan_plaza_authority_(target.plaza->clan_id, members,
                                   [&](const auto& trusted) {
                                     scope = trusted;
                                     return commit();
                                   });
    };
  }
  // Party validation and all of its packet encoding precede this callback.
  // A scene failure leaves party and existing plaza membership unchanged.
  const auto status = input.membership->JoinApproved(
      *target.party,
      [&] {
        if (target.plaza) {
          auto admission =
              plazas_->EnterGroup(input.envelope,
                                  {{player, joiner.account_id, input.scene_sink,
                                    input.plaza_source_token}},
                                  target.plaza, scope);
          if (admission.status != lobby::PlazaAdmissionStatus::kEntered)
            return false;
          result.plaza = std::move(admission.members.front());
        } else {
          auto envelope = input.envelope;
          envelope.descriptor.session_type = 1;
          std::vector<protocol::TcpPacket> packets{
              scenes::SceneRoster::Compose(
                  std::move(envelope), std::array{player}, 0, joiner.account_id)
                  .Encode()};
          // Enqueue-only publication has the same closed-connection semantics
          // as registry sinks. The accepting session releases its old plaza
          // lease.
          try {
            input.scene_sink(std::move(packets));
          } catch (...) {
          }
          result.moved_to_menu = true;
        }
        return true;
      },
      authority);
  using parties::PartyJoinStatus;
  if (status != PartyJoinStatus::kJoined) {
    // Native 9A9CD0 maps 2 to SameParty, 8 to Ingame and 14 to FullParty.
    fail(status == PartyJoinStatus::kAlreadyJoined ? 2U
         : status == PartyJoinStatus::kBusy        ? 8U
         : status == PartyJoinStatus::kFull        ? 14U
                                                   : 21U);
  }
  return result;
}
}  // namespace bubble_fighter::server::features::social
