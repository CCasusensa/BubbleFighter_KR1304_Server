#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_TRANSFERS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_TRANSFERS_H_

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "features/lobby/plaza_registry.h"
#include "features/parties/party_registry.h"
#include "features/rooms/room_creation.h"
#include "protocol/rooms/join_room_request.h"

namespace bubble_fighter::server::features::parties {
enum class PartySceneKind {
  kMenu,
  kPlaza,
  kCreateRoom,
  kJoinRoom,
  kLeaveRoom,
  kClanPlaza
};
struct PartySceneRequest {
  PartySceneKind kind = PartySceneKind::kPlaza;
  protocol::CreateRoomRequest create;
  protocol::JoinRoomRequest join;
  std::uint32_t clan_id = 0;
};
struct PartySceneWork {
  std::uint64_t id = 0;
  PartySceneRequest request;
};
// Constructed only by the participant's own session. The old plaza lease is
// held here until commit or adoption of a failed outcome restores it.
struct PartyScenePrepared {
  characters::CharacterRecord character;
  protocol::GotoRoomNotification envelope;
  protocol::SessionMember member;
  protocol::PartySupplementaryRecord equipment;
  lobby::PlazaLease source;
  rooms::RoomLease source_room;
  // Immutable after Prepare, including while another connection commits.
  std::uint16_t source_room_id = 0;
  std::vector<protocol::TcpPacket> menu_packets;
};
struct PartySceneOutcome {
  PartySceneKind kind = PartySceneKind::kPlaza;
  bool committed = false;
  std::shared_ptr<PartyScenePrepared> prepared;
  rooms::RoomLease room;
  lobby::PlazaLease plaza;
  std::shared_ptr<rooms::RoomPacketQueue> packets;
  std::vector<protocol::TcpPacket> refusal;
};
class PartySceneTransfers;
class PartySceneConnection {
 public:
  using Time = std::chrono::steady_clock::time_point;
  PartySceneConnection() = default;
  PartySceneConnection(PartySceneConnection&& other) noexcept;
  PartySceneConnection& operator=(PartySceneConnection&& other) noexcept;
  PartySceneConnection(const PartySceneConnection&) = delete;
  PartySceneConnection& operator=(const PartySceneConnection&) = delete;
  ~PartySceneConnection();
  explicit operator bool() const noexcept;
  void Release() noexcept;
  bool Begin(const PartyMembership& leader, PartySceneRequest request,
             Time now);
  bool Pending() const;
  std::optional<PartySceneWork> Work(Time now);
  bool Prepare(std::uint64_t id, std::shared_ptr<PartyScenePrepared> prepared,
               Time now);
  void Cancel() noexcept;
  std::optional<PartySceneOutcome> Take(Time now);

 private:
  friend class PartySceneTransfers;
  PartySceneConnection(std::shared_ptr<PartySceneTransfers> registry,
                       std::uint64_t token) noexcept;
  std::shared_ptr<PartySceneTransfers> registry_;
  std::uint64_t token_ = 0;
};

// All calls enqueue/collect data; this service never invokes a PlayerSession.
// Lock order: transfers -> party -> clan operations -> completed database read
// -> room -> plaza. Owning sessions
// invalidate invitations before Prepare, outside this service's lock.
class PartySceneTransfers
    : public std::enable_shared_from_this<PartySceneTransfers> {
 public:
  using Time = PartySceneConnection::Time;
  PartySceneTransfers(std::shared_ptr<PartyRegistry> parties,
                      std::shared_ptr<rooms::RoomRegistry> rooms,
                      std::shared_ptr<lobby::PlazaRegistry> plazas,
                      clans::ClanPlazaAuthority clan_plaza_authority = {});
  PartySceneConnection Register(const PartyMembership& member);

 private:
  friend class PartySceneConnection;
  struct Participant {
    PartySceneMember identity;
    std::shared_ptr<PartyScenePrepared> prepared;
    std::shared_ptr<rooms::RoomPacketQueue> packets;
    rooms::RoomLease room;
    lobby::PlazaLease plaza;
  };
  struct Operation {
    std::uint64_t id = 0;
    PartySceneRequest request;
    Time deadline;
    std::optional<PartySceneReservation> reservation;
    std::vector<Participant> participants;
    std::vector<protocol::TcpPacket> refusal;
    bool finished = false, committed = false;
  };
  bool Begin(std::uint64_t token, const PartyMembership& leader,
             PartySceneRequest request, Time now);
  std::optional<PartySceneWork> Work(std::uint64_t token, Time now);
  bool Prepare(std::uint64_t token, std::uint64_t id,
               std::shared_ptr<PartyScenePrepared> prepared, Time now);
  std::optional<PartySceneOutcome> Take(std::uint64_t token, Time now);
  bool Pending(std::uint64_t token) const;
  void Cancel(std::uint64_t token, bool disconnect) noexcept;
  static void Expire(Operation& operation, Time now) noexcept;
  static Participant& Find(Operation& operation, std::uint64_t token);
  void Commit(Operation& operation);
  bool CommitScene(Operation& operation,
                   const clans::ClanPlazaScope& scope = {});
  std::shared_ptr<PartyRegistry> parties_;
  std::shared_ptr<rooms::RoomRegistry> rooms_;
  std::shared_ptr<lobby::PlazaRegistry> plazas_;
  clans::ClanPlazaAuthority clan_plaza_authority_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_ = 1;
  std::map<std::uint64_t, std::shared_ptr<Operation>> connections_;
};
}  // namespace bubble_fighter::server::features::parties

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PARTIES_PARTY_SCENE_TRANSFERS_H_
