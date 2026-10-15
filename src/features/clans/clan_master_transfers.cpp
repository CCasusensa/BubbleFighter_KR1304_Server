#include "features/clans/clan_master_transfers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bubble_fighter::server::features::clans {
using namespace protocol;
namespace {
ClanDelivery Deliver(std::uint32_t character, ClanPacket packet) {
  ClanPacketCodec::Encode(packet);
  return {{character},
          std::make_shared<const std::vector<ClanPacket>>(
              std::vector{std::move(packet)})};
}
ClanMutationResult RefuseOffer() {
  return {{ClanMasterTransferResponse{0, 0}.Encode()}, {}};
}
ClanMutationResult RefuseAnswer() {
  return {{ClanMasterTransferAnswerResponse{0}.Encode()}, {}};
}
}  // namespace

ClanMasterTransfers::ClanMasterTransfers(
    std::shared_ptr<Authentication> authentication,
    ClanMasterTransferSource source, ClanMasterTransferCommitter commit,
    ClanProfileSource profiles, Authentication::Now now)
    : authentication_(std::move(authentication)),
      source_(std::move(source)),
      commit_(std::move(commit)),
      profiles_(std::move(profiles)),
      now_(std::move(now)) {
  if (!authentication_ || !source_ || !commit_ || !profiles_ || !now_)
    throw std::invalid_argument("Missing clan master transfer dependencies.");
}
bool ClanMasterTransfers::Current(const Pending& pending) const {
  return now_() < pending.deadline &&
         pending.requester.Identity() ==
             std::optional{pending.parties.requester} &&
         pending.target.Identity() == std::optional{pending.parties.target} &&
         pending.requester.PlazaEpoch() ==
             std::optional{pending.requester_epoch} &&
         pending.target.PlazaEpoch() == std::optional{pending.target_epoch};
}
ClanMutationResult ClanMasterTransfers::RefuseCommit(
    const Authentication::Binding& binding) {
  const auto identity = binding.Identity();
  const auto profile = identity ? profiles_(*identity) : std::nullopt;
  if (!profile || !binding.Identity())
    throw std::runtime_error("Clan master transfer owner is unavailable.");
  return {
      {ClanMasterTransferResponse{0, 0}.Encode(), profile->profile.Encode()},
      {}};
}
ClanMutationResult ClanMasterTransfers::Process(
    const Authentication::Binding& binding,
    const ClanMasterTransferAction& action) {
  std::lock_guard lock(mutex_);
  std::visit([](const auto& value) { value.Encode(); }, action);
  std::erase_if(pending_,
                [this](const auto& item) { return !Current(item.second); });
  const auto identity = binding.Identity();
  if (!identity)
    throw std::runtime_error("Clan master transfer owner is unavailable.");
  if (const auto request = std::get_if<ClanMasterTransferRequest>(&action)) {
    const auto requester_epoch = binding.PlazaEpoch();
    auto target = authentication_->Find(request->character_id);
    const auto target_epoch = target.PlazaEpoch();
    if (!requester_epoch || !target_epoch ||
        request->character_id == identity->character_id ||
        pending_.size() >= 64 ||
        next_key_ > std::numeric_limits<std::uint32_t>::max())
      return RefuseOffer();
    const auto parties = source_(*identity, request->character_id);
    if (!parties || parties->requester != *identity ||
        target.Identity() != std::optional{parties->target})
      return RefuseOffer();
    for (const auto& [key, pending] : pending_) {
      const auto& existing = pending.parties;
      if (existing.clan_id == parties->clan_id ||
          existing.requester == parties->requester ||
          existing.target == parties->requester ||
          existing.requester == parties->target ||
          existing.target == parties->target)
        return RefuseOffer();
    }
    Pending pending{*parties,
                    binding,
                    std::move(target),
                    *requester_epoch,
                    *target_epoch,
                    now_() + std::chrono::seconds(60),
                    false};
    if (!Current(pending)) return RefuseOffer();
    const auto key = static_cast<std::uint32_t>(next_key_);
    ClanMutationResult result{{ClanMasterTransferResponse{1, 0}.Encode()},
                              {Deliver(parties->target.character_id,
                                       ClanMasterTransferOfferNotification{
                                           parties->requester_name, key}
                                           .Encode())}};
    pending_.emplace(key, std::move(pending));
    ++next_key_;
    return result;
  }
  if (const auto answer =
          std::get_if<ClanMasterTransferAnswerRequest>(&action)) {
    const auto found = pending_.find(answer->correlation_value);
    if (answer->accepted > 1 || found == pending_.end() ||
        found->second.accepted || !found->second.target.SameConnection(binding))
      return RefuseAnswer();
    auto& pending = found->second;
    const auto fresh =
        source_(pending.parties.requester, pending.parties.target.character_id);
    if (fresh != std::optional{pending.parties} || !Current(pending)) {
      pending_.erase(found);
      return RefuseAnswer();
    }
    ClanMutationResult result{
        {ClanMasterTransferAnswerResponse{1}.Encode()},
        {Deliver(pending.parties.requester.character_id,
                 ClanMasterTransferDecisionNotification{
                     answer->accepted, pending.parties.target.character_id,
                     pending.parties.target_name}
                     .Encode())}};
    if (answer->accepted)
      pending.accepted = true;
    else
      pending_.erase(found);
    return result;
  }
  const auto& request = std::get<ClanMasterTransferCommitRequest>(action);
  const auto found =
      std::find_if(pending_.begin(), pending_.end(), [&](const auto& item) {
        return item.second.requester.SameConnection(binding);
      });
  if (found == pending_.end()) return RefuseCommit(binding);
  auto pending = std::move(found->second);
  pending_.erase(found);  // One use, including stale/mismatched/failed commits.
  if (!pending.accepted || !Current(pending) ||
      request.character_id != pending.parties.target.character_id ||
      request.character_name != pending.parties.target_name)
    return RefuseCommit(binding);
  return commit_(pending.parties,
                 [this, &pending] { return Current(pending); });
}
}  // namespace bubble_fighter::server::features::clans
