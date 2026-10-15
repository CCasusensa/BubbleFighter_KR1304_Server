#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_ROOM_INVITATIONS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_ROOM_INVITATIONS_H_

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

#include "features/lobby/plaza_registry.h"
#include "features/parties/party_registry.h"
#include "features/rooms/room_registry.h"
#include "protocol/social/invitation_messages.h"
#include "protocol/social/visitation_messages.h"

namespace bubble_fighter::server::features::social {
class RoomInvitations;
struct InvitationAdmission {
  rooms::RoomLease room;
  std::vector<protocol::TcpPacket> responses;
  lobby::PlazaLease plaza;
  bool moved_to_menu = false;
};
// Prepared on the accepting connection's thread, never retained in the table.
struct PartyAdmissionInput {
  parties::PartyMembership* membership = nullptr;
  protocol::GotoRoomNotification envelope;
  rooms::RoomPacketSink scene_sink;
  std::uint64_t plaza_source_token = 0;
  parties::PartyGroupAuthority authority;
};

class InvitationLease {
 public:
  using Time = std::chrono::steady_clock::time_point;
  InvitationLease() = default;
  InvitationLease(InvitationLease&& other) noexcept;
  InvitationLease& operator=(InvitationLease&& other) noexcept;
  InvitationLease(const InvitationLease&) = delete;
  InvitationLease& operator=(const InvitationLease&) = delete;
  ~InvitationLease();
  explicit operator bool() const noexcept;
  void Release() noexcept;
  void Update(const characters::CharacterRecord& character, bool available);
  // Before a scene/identity transition: revoke incoming and outgoing party
  // authority; reject new targets until Update completes the change. Outgoing
  // room consent retains the existing playing/stale refusal precedence.
  void Invalidate();
  std::vector<protocol::TcpPacket> Invite(
      const protocol::InvitationRequest& request, Time now);
  void Decline(std::uint32_t key, Time now);
  std::vector<protocol::TcpPacket> RequestVisit(
      const protocol::VisitationRequest& request, Time now);
  void DeclineVisit(const protocol::DeclineVisitationRequest& request,
                    Time now);
  void ApproveVisit(std::uint32_t key, Time now);
  InvitationAdmission Accept(const protocol::MoveToInviterRequest& request,
                             rooms::RoomOccupant joiner,
                             rooms::RoomPacketSink sink, Time now,
                             const PartyAdmissionInput* party_input = nullptr);

 private:
  friend class RoomInvitations;
  InvitationLease(std::shared_ptr<RoomInvitations> registry,
                  std::uint64_t token);
  std::shared_ptr<RoomInvitations> registry_;
  std::uint64_t token_ = 0;
};

// Authenticated room and menu/plaza party consent. Lock order: invitations ->
// parties -> scene table / block cache. Sinks may only enqueue packets.
class RoomInvitations : public std::enable_shared_from_this<RoomInvitations> {
 public:
  using Time = InvitationLease::Time;
  using AllowsPair = std::function<bool(std::uint64_t, std::uint64_t)>;
  explicit RoomInvitations(std::shared_ptr<rooms::RoomRegistry> rooms,
                           AllowsPair allows_pair = {},
                           std::shared_ptr<parties::PartyRegistry> parties = {},
                           std::shared_ptr<lobby::PlazaRegistry> plazas = {},
                           clans::ClanPlazaAuthority clan_plaza_authority = {});
  InvitationLease Enter(const characters::CharacterRecord& character,
                        rooms::RoomPacketSink sink, bool available);

 private:
  friend class InvitationLease;
  struct Member {
    characters::CharacterRecord character;
    rooms::RoomPacketSink sink;
    bool available = false;
    bool transitioning = false;
  };
  struct Destination {
    rooms::RoomInvitationTarget room;
    std::optional<parties::PartyTarget> party;
    std::optional<lobby::PlazaTarget> plaza;
  };
  struct Pending {
    enum class Kind { kInvitation, kVisitRequest, kApprovedVisit };
    std::uint64_t sender = 0;
    std::uint64_t recipient = 0;
    Destination target;
    Time expires;
    Kind kind = Kind::kInvitation;
  };
  void Release(std::uint64_t token) noexcept;
  void Invalidate(std::uint64_t token);
  void Update(std::uint64_t token, const characters::CharacterRecord& character,
              bool available);
  std::vector<protocol::TcpPacket> Invite(
      std::uint64_t token, const protocol::InvitationRequest& request,
      Time now);
  void Decline(std::uint64_t token, std::uint32_t key, Time now);
  std::vector<protocol::TcpPacket> RequestVisit(
      std::uint64_t token, const protocol::VisitationRequest& request,
      Time now);
  void DecideVisit(std::uint64_t token, std::uint32_t key, bool approved,
                   Time now);
  InvitationAdmission Accept(std::uint64_t token,
                             const protocol::MoveToInviterRequest& request,
                             rooms::RoomOccupant joiner,
                             rooms::RoomPacketSink sink, Time now,
                             const PartyAdmissionInput* party_input);
  std::optional<Destination> CaptureTarget(const Member& owner) const;
  std::uint32_t CheckTarget(const Destination& target,
                            const Member& owner) const;
  InvitationAdmission AcceptParty(const Destination& target,
                                  rooms::RoomOccupant joiner,
                                  const PartyAdmissionInput& input);
  void Expire(Time now);
  bool Allows(const Member& sender, const Member& recipient) const;
  static void Deliver(const Member& member,
                      protocol::TcpPacket packet) noexcept;
  std::shared_ptr<rooms::RoomRegistry> rooms_;
  std::shared_ptr<parties::PartyRegistry> parties_;
  std::shared_ptr<lobby::PlazaRegistry> plazas_;
  AllowsPair allows_pair_;
  clans::ClanPlazaAuthority clan_plaza_authority_;
  std::mutex mutex_;
  std::uint64_t next_token_ = 1;
  std::uint64_t next_key_ = 1;
  std::map<std::uint64_t, Member> members_;
  std::map<std::uint32_t, Pending> pending_;
};
}  // namespace bubble_fighter::server::features::social

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_ROOM_INVITATIONS_H_
