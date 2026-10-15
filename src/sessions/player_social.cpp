#include <stdexcept>

#include "sessions/player_session.h"

namespace bubble_fighter::server::sessions {
std::optional<std::vector<protocol::TcpPacket>> PlayerSession::ProcessSocial(
    const protocol::TcpPacket& packet) {
  using namespace protocol;
  if (packet.message_id != CompanionSocialRequest::kMessageId &&
      packet.message_id != CompanionFriendGroupRequest::kMessageId &&
      packet.message_id != CompanionMailboxRequest::kMessageId &&
      packet.message_id != 147 && packet.message_id != 148) {
    return std::nullopt;
  }
  if (!social_lease_ || (!CommerceInitialized() && !practice_)) {
    throw std::runtime_error(
        "Social request before authenticated initialization.");
  }
  if (packet.message_id == CompanionMailboxRequest::kMessageId) {
    const auto request = CompanionMailboxRequest::Decode(packet);
    // Acknowledgements can consume only the bounded issued batch. Applying the
    // five-action send quota would prevent acknowledging a twenty-message page.
    if (request.action == CompanionMailboxAction::kFetch &&
        !chat_.Admit(practice_clock_())) {
      return std::vector<TcpPacket>{CompanionMailboxNotification{
          CompanionMailboxStatus::kRateLimited,
          {}}.Encode()};
    }
    social_lease_.ProcessMailbox(request);
  } else if (packet.message_id == CompanionFriendGroupRequest::kMessageId) {
    const auto request = CompanionFriendGroupRequest::Decode(packet);
    if (!chat_.Admit(practice_clock_())) {
      return std::vector<TcpPacket>{CompanionFriendGroupsNotification{
          CompanionFriendGroupStatus::kInvalid,
          request.action,
          {},
          {}}.Encode()};
    }
    social_lease_.ProcessGroups(request);
  } else if (packet.message_id == CompanionSocialRequest::kMessageId) {
    const auto request = CompanionSocialRequest::Decode(packet);
    if (!chat_.Admit(practice_clock_())) {
      return std::vector<TcpPacket>{CompanionSocialRosterNotification{
          CompanionSocialStatus::kInvalid,
          request.action,
          u"操作过于频繁，请稍后重试。",
          {}}.Encode()};
    }
    social_lease_.Process(request);
  } else {
    const auto request = ChatBlockRequest::Decode(packet);
    if (!chat_.Admit(practice_clock_())) {
      return std::vector<TcpPacket>{
          features::chat::SceneChat::Notice(u"操作过于频繁，请稍后重试。")};
    }
    social_lease_.Block(request);
  }
  return std::vector<TcpPacket>{};
}
}  // namespace bubble_fighter::server::sessions
