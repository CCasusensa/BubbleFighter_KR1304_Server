#include "sessions/clan_session.h"

#include <iterator>
#include <stdexcept>
#include <utility>

#include "protocol/clans/clan_presence_report.h"

namespace bubble_fighter::server::sessions {
using namespace protocol;
using network::ClanHandlingResult;

ClanSession::ClanSession(
    std::shared_ptr<Authentication> authentication,
    features::clans::ClanProfileSource profiles, Authentication::Now now,
    features::clans::ClanCreateHandler create,
    features::clans::ClanDirectorySource directory,
    features::clans::ClanRosterSource rosters,
    features::clans::ClanMutationHandler mutate,
    std::shared_ptr<features::clans::ClanMasterTransfers> master_transfers,
    features::clans::ClanDisbandHandler disband,
    features::clans::TrainingClanHandler training)
    : authentication_(std::move(authentication)),
      profiles_(std::move(profiles)),
      now_(std::move(now)),
      create_(std::move(create)),
      directory_(std::move(directory)),
      rosters_(std::move(rosters)),
      mutate_(std::move(mutate)),
      master_transfers_(std::move(master_transfers)),
      disband_(std::move(disband)),
      training_(std::move(training)) {
  if (!authentication_ || !profiles_ || !now_)
    throw std::invalid_argument("Missing clan session dependencies.");
  deadline_ = now_() + std::chrono::seconds{10};
}

ClanHandlingResult ClanSession::Check() {
  if (closed_ || (authenticated_ && !binding_.Identity()) ||
      (!authenticated_ && now_() >= deadline_)) {
    closed_ = true;
    binding_ = {};
    return {{}, true};
  }
  return {};
}

std::optional<features::clans::ClanSnapshot> ClanSession::Load() {
  const auto identity = binding_.Identity();
  if (!identity) return {};
  auto snapshot = profiles_(*identity);
  // Database work never pins a main session. Check revocation again before
  // publishing a result from a potentially slow database read.
  if (!binding_.Identity()) return {};
  return snapshot;
}

ClanHandlingResult ClanSession::Poll() {
  auto operation = authentication_->LockOperations();
  if (auto state = Check(); state.close_connection) return state;
  if (!authenticated_) return {};
  auto packets = binding_.TakeNotifications();
  if (!packets) {
    closed_ = true;
    binding_ = {};
    return {{}, true};
  }
  return {std::move(*packets), false};
}

ClanHandlingResult ClanSession::Process(const ClanPacket& packet) {
  auto operation = authentication_->LockOperations();
  if (auto state = Check(); state.close_connection) return state;
  std::vector<ClanPacket> pending;
  if (authenticated_) {
    auto queued = binding_.TakeNotifications();
    if (!queued) {
      closed_ = true;
      binding_ = {};
      return {{}, true};
    }
    pending = std::move(*queued);
  }
  auto result = ProcessPacket(packet);
  if (result.close_connection) return result;
  // Older queued events must precede the fresh response, including profile
  // reads. Successful reauthentication has a new empty inbox/generation.
  pending.insert(pending.end(),
                 std::make_move_iterator(result.responses.begin()),
                 std::make_move_iterator(result.responses.end()));
  result.responses = std::move(pending);
  return result;
}

ClanHandlingResult ClanSession::ProcessPacket(const ClanPacket& packet) {
  try {
    if (packet.message_id == ClanHeartbeatRequest::kMessageId) {
      ClanHeartbeatRequest::Decode(packet);
      return {};  // Does not extend the pre-authentication deadline.
    }
    if (!authenticated_) {
      if (packet.message_id != ClanLoginRequest::kMessageId)
        throw std::runtime_error("Clan packet before authentication.");
      binding_ = authentication_->Consume(
          ClanLoginRequest::Decode(packet).credentials);
      if (!Load() || now_() >= deadline_) {
        closed_ = true;
        binding_ = {};
        return {{ClanLoginResponse{0xffffffffU}.Encode()}, true};
      }
      authenticated_ = true;
      return {{ClanLoginResponse{1}.Encode()}, false};
    }
    if (packet.message_id == ClanPresenceReport::kMessageId) {
      ClanPresenceReport::Decode(packet);
      // The main session is the only scene publisher. Native reports can be
      // delayed or forged and have no authority over presence or membership.
      return {};
    }
    if (packet.message_id == ClanProfileRequest::kMessageId ||
        packet.message_id == ClanDetailsRequest::kMessageId) {
      const bool details = packet.message_id == ClanDetailsRequest::kMessageId;
      if (details)
        ClanDetailsRequest::Decode(packet);
      else
        ClanProfileRequest::Decode(packet);
      const auto snapshot = Load();
      if (!snapshot) {
        closed_ = true;
        binding_ = {};
        return {{}, true};
      }
      if (snapshot->disband_notice &&
          delivered_disband_ != snapshot->disband_notice->clan_id) {
        delivered_disband_ = snapshot->disband_notice->clan_id;
        return {
            {snapshot->disband_notice->Encode(), snapshot->profile.Encode()},
            false};
      }
      if (snapshot->training_exit_notice &&
          delivered_training_ != snapshot->training_exit_generation) {
        delivered_training_ = snapshot->training_exit_generation;
        return {{snapshot->training_exit_notice->Encode(),
                 snapshot->profile.Encode()},
                false};
      }
      // A stale details query after membership loss resynchronizes S1016.
      // This is project ordering, not a recovered original-server policy.
      return {{details && snapshot->details ? snapshot->details->Encode()
                                            : snapshot->profile.Encode()},
              false};
    }
    if (packet.message_id == ClanDirectoryRequest::kMessageId) {
      const auto request = ClanDirectoryRequest::Decode(packet);
      if (!directory_ ||
          !features::clans::IsClanDirectoryQuerySupported(request))
        throw std::runtime_error("Unsupported clan directory query.");
      const auto identity = binding_.Identity();
      const auto response =
          identity ? directory_(*identity, request) : std::nullopt;
      if (!response || !binding_.Identity()) {
        closed_ = true;
        binding_ = {};
        return {{}, true};
      }
      return {{response->Encode()}, false};
    }
    if (packet.message_id == ClanRosterRequest::kMessageId ||
        packet.message_id == ClanMemberDetailsRequest::kMessageId ||
        packet.message_id == ClanApplicantsRequest::kMessageId) {
      using features::clans::ClanRosterKind;
      if (!rosters_)
        throw std::runtime_error("Clan roster source is unavailable.");
      auto kind = ClanRosterKind::kMembers;
      if (packet.message_id == ClanRosterRequest::kMessageId) {
        ClanRosterRequest::Decode(packet);
      } else if (packet.message_id == ClanMemberDetailsRequest::kMessageId) {
        if (ClanMemberDetailsRequest::Decode(packet).value_at0 != 1)
          throw std::runtime_error("Unsupported clan roster query.");
        kind = ClanRosterKind::kDetails;
      } else {
        ClanApplicantsRequest::Decode(packet);
        kind = ClanRosterKind::kApplicants;
      }
      const auto identity = binding_.Identity();
      auto response = identity ? rosters_(*identity, kind) : std::nullopt;
      if (!response || !binding_.Identity()) {
        closed_ = true;
        binding_ = {};
        return {{}, true};
      }
      // Validate the complete publication before returning any packet.
      for (auto& reply : *response) {
        if (reply.message_id == ClanRosterResponse::kMessageId) {
          auto roster = ClanRosterResponse::Decode(reply);
          std::vector<std::uint32_t> characters;
          characters.reserve(roster.members.size());
          for (const auto& member : roster.members)
            characters.push_back(member.character_id);
          const auto scenes = authentication_->ReadScenes(characters);
          for (std::size_t i = 0; i < scenes.size(); ++i)
            roster.members[i].value_at36 = scenes[i];
          reply = roster.Encode();
        }
        ClanPacketCodec::Encode(reply);
      }
      return {std::move(*response), false};
    }
    if (packet.message_id == ClanDisbandRequest::kMessageId) {
      const auto request = ClanDisbandRequest::Decode(packet);
      const auto identity = binding_.Identity();
      const auto epoch = binding_.PlazaEpoch();
      if (!identity) return {{}, true};
      if (!disband_ || !epoch)
        return {{ClanDisbandResponse{0, request.action}.Encode()}, false};
      auto result = disband_(*identity, request, [this, epoch] {
        return binding_.PlazaEpoch() == epoch &&
               binding_.Identity().has_value();
      });
      authentication_->InvalidateMembership();
      authentication_->Publish(result.deliveries);
      if (!binding_.Identity()) return {{}, true};
      return {std::move(result.responses), false};
    }
    std::optional<features::clans::ClanMasterTransferAction> transfer;
    if (packet.message_id == ClanMasterTransferRequest::kMessageId)
      transfer = ClanMasterTransferRequest::Decode(packet);
    else if (packet.message_id == ClanMasterTransferAnswerRequest::kMessageId)
      transfer = ClanMasterTransferAnswerRequest::Decode(packet);
    else if (packet.message_id == ClanMasterTransferCommitRequest::kMessageId)
      transfer = ClanMasterTransferCommitRequest::Decode(packet);
    if (transfer) {
      if (!master_transfers_)
        throw std::runtime_error("Clan master transfer source is unavailable.");
      auto result = master_transfers_->Process(binding_, *transfer);
      authentication_->InvalidateMembership();
      authentication_->Publish(result.deliveries);
      if (!binding_.Identity()) {
        closed_ = true;
        binding_ = {};
        return {{}, true};
      }
      return {std::move(result.responses), false};
    }
    bool manual_leave = false;
    if (packet.message_id == ClanLeaveRequest::kMessageId) {
      ClanLeaveRequest::Decode(packet);
      manual_leave = true;
    } else if (packet.message_id == TrainingClanExitRequest::kMessageId) {
      manual_leave =
          TrainingClanExitRequest::Decode(packet).new_status == 0xffffffecU;
    }
    if (manual_leave && binding_.InClanChannel()) {
      const auto snapshot = Load();
      if (!snapshot) return {{}, true};
      return {{snapshot->profile.Encode()}, false};
    }
    if (packet.message_id == TrainingClanJoinRequest::kMessageId ||
        packet.message_id == TrainingClanExitRequest::kMessageId ||
        (training_ && packet.message_id == ClanLeaveRequest::kMessageId)) {
      using features::clans::TrainingClanAction;
      auto action = TrainingClanAction::kManualLeave;
      if (packet.message_id == TrainingClanJoinRequest::kMessageId) {
        TrainingClanJoinRequest::Decode(packet);
        action = TrainingClanAction::kJoin;
      } else if (packet.message_id == TrainingClanExitRequest::kMessageId) {
        const auto status = TrainingClanExitRequest::Decode(packet).new_status;
        action = status == 0xffffffecU   ? TrainingClanAction::kManualLeave
                 : status == 0xffffffc4U ? TrainingClanAction::kGraduate
                                         : TrainingClanAction::kUnsupported;
      } else {
        ClanLeaveRequest::Decode(packet);
      }
      if (!training_)
        throw std::runtime_error("Training clan handler is unavailable.");
      const auto identity = binding_.Identity();
      if (!identity) return {{}, true};
      auto result = training_(*identity, action, [this, manual_leave] {
        return binding_.Identity().has_value() &&
               (!manual_leave || !binding_.InClanChannel());
      });
      authentication_->InvalidateMembership();
      if (!binding_.Identity()) return {{}, true};
      if (result.handled || packet.message_id != ClanLeaveRequest::kMessageId) {
        authentication_->Publish(result.mutation.deliveries);
        return {std::move(result.mutation.responses), false};
      }
    }
    std::optional<features::clans::ClanMutation> mutation;
    switch (packet.message_id) {
      case ClanJoinRequest::kMessageId:
        mutation = ClanJoinRequest::Decode(packet);
        break;
      case ClanAutoJoinRequest::kMessageId:
        mutation = ClanAutoJoinRequest::Decode(packet);
        break;
      case ClanCancelJoinRequest::kMessageId:
        mutation = ClanCancelJoinRequest::Decode(packet);
        break;
      case ClanLeaveRequest::kMessageId:
        mutation = ClanLeaveRequest::Decode(packet);
        break;
      case ClanMemberChangeRequest::kMessageId:
        mutation = ClanMemberChangeRequest::Decode(packet);
        break;
      case ClanRecruitmentRequest::kMessageId:
        mutation = ClanRecruitmentRequest::Decode(packet);
        break;
      default:
        break;
    }
    if (mutation) {
      if (!mutate_)
        throw std::runtime_error("Clan mutation source is unavailable.");
      const auto identity = binding_.Identity();
      if (!identity) return {{}, true};
      auto result = mutate_(*identity, *mutation, [this, manual_leave] {
        return binding_.Identity().has_value() &&
               (!manual_leave || !binding_.InClanChannel());
      });
      authentication_->InvalidateMembership();
      // Store prepares encoded immutable notification batches before commit.
      // Publishing only copies shared pointers into bounded per-generation
      // inboxes.
      authentication_->Publish(result.deliveries);
      if (!binding_.Identity()) {
        closed_ = true;
        binding_ = {};
        return {{}, true};
      }
      return {std::move(result.responses), false};
    }
    if (packet.message_id == ClanCreateRequest::kMessageId) {
      const auto request = ClanCreateRequest::Decode(packet);
      if (!create_) return {{ClanCreateResponse{}.Encode()}, false};
      const auto identity = binding_.Identity();
      if (!identity) return {{}, true};
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      if (seconds < 0)
        throw std::runtime_error("Server clock predates the Unix epoch.");
      const auto response =
          create_(*identity, request, static_cast<std::uint64_t>(seconds),
                  [this] { return binding_.Identity().has_value(); });
      authentication_->InvalidateMembership();
      return {{response.Encode()}, false};
    }
    throw std::runtime_error("Unexpected authenticated clan packet.");
  } catch (...) {
    closed_ = true;
    binding_ = {};
    throw;
  }
}
}  // namespace bubble_fighter::server::sessions
