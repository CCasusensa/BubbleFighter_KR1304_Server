#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_PARTY_CHANNEL_TRANSFERS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_PARTY_CHANNEL_TRANSFERS_H_

#include <chrono>

#include "features/lobby/plaza_registry.h"
#include "features/parties/party_registry.h"
#include "features/rooms/room_registry.h"

namespace bubble_fighter::server::features::channels {
struct ChannelWorld;
enum class ChannelMovePhase {
  kIdle,
  kPreparing,
  kWaiting,
  kOffer,
  kConnecting,
  kSwitch,
  kCommitted,
  kCancelled,
  kClosed
};
enum class ChannelMoveSignal { kOffered, kEntered, kSwitched, kAcknowledged };
struct ChannelMoveStatus {
  ChannelMovePhase phase = ChannelMovePhase::kIdle;
  std::uint64_t id = 0;
  std::uint32_t category = 0;
  bool initiator = false;
  bool irreversible = false;
  std::shared_ptr<const ChannelWorld> destination;
};
struct ChannelMovePrepared {
  characters::CharacterRecord character;
  protocol::GotoRoomNotification envelope;
  protocol::SessionMember member;
  protocol::PartySupplementaryRecord equipment;
  lobby::PlazaLease source;
  rooms::RoomLease source_room;
  std::uint16_t source_room_id = 0;
  std::shared_ptr<rooms::RoomPacketQueue> packets;
};
struct ChannelMoveOutcome {
  bool committed = false;
  bool closed = false;
  std::shared_ptr<ChannelMovePrepared> prepared;
  lobby::PlazaLease plaza;
};
class PartyChannelTransfers;
class PartyChannelConnection {
 public:
  using Time = std::chrono::steady_clock::time_point;
  PartyChannelConnection() = default;
  PartyChannelConnection(PartyChannelConnection&& other) noexcept;
  PartyChannelConnection& operator=(PartyChannelConnection&& other) noexcept;
  PartyChannelConnection(const PartyChannelConnection&) = delete;
  PartyChannelConnection& operator=(const PartyChannelConnection&) = delete;
  ~PartyChannelConnection();
  void Release() noexcept;
  void Enable();
  void SetCategory(std::uint32_t category);
  bool Begin(const parties::PartyMembership& leader,
             std::shared_ptr<const ChannelWorld> source,
             std::shared_ptr<const ChannelWorld> destination, Time now);
  ChannelMoveStatus Status(Time now) const;
  bool Pending() const;
  bool Irreversible() const;
  std::uint32_t Refusal() const;
  bool Prepare(std::uint64_t id, std::shared_ptr<ChannelMovePrepared> prepared,
               Time now);
  bool Signal(ChannelMoveSignal signal, Time now);
  void Cancel() noexcept;
  std::optional<ChannelMoveOutcome> Take(Time now);

 private:
  friend class PartyChannelTransfers;
  PartyChannelConnection(std::shared_ptr<PartyChannelTransfers> registry,
                         std::uint64_t token) noexcept;
  std::shared_ptr<PartyChannelTransfers> registry_;
  std::uint64_t token_ = 0;
};
// Sessions prepare/collect their own state. No callbacks into other sessions.
// Lock order: coordinator -> party -> clan operations -> source rooms -> plaza.
class PartyChannelTransfers
    : public std::enable_shared_from_this<PartyChannelTransfers> {
 public:
  using Time = PartyChannelConnection::Time;
  explicit PartyChannelTransfers(
      std::shared_ptr<parties::PartyRegistry> parties);
  PartyChannelConnection Register(const parties::PartyMembership& member,
                                  std::uint32_t category);

 private:
  friend class PartyChannelConnection;
  struct Participant {
    parties::PartySceneMember identity;
    std::shared_ptr<ChannelMovePrepared> prepared;
    lobby::PlazaLease plaza;
    bool offered = false, entered = false, switched = false,
         acknowledged = false;
  };
  struct Operation {
    std::uint64_t id = 0;
    Time deadline;
    std::shared_ptr<const ChannelWorld> source, destination;
    std::optional<parties::PartySceneReservation> reservation;
    std::vector<Participant> participants;
    bool finished = false, committed = false, irreversible = false;
  };
  struct Connection {
    std::uint32_t category = 0;
    bool capable = false;
    std::shared_ptr<Operation> operation;
    std::uint32_t refusal = 2;
  };
  static Participant& Find(Operation& operation, std::uint64_t token);
  static void Expire(Operation& operation, Time now) noexcept;
  bool Begin(std::uint64_t token, const parties::PartyMembership& leader,
             std::shared_ptr<const ChannelWorld> source,
             std::shared_ptr<const ChannelWorld> destination, Time now);
  ChannelMoveStatus Status(std::uint64_t token, Time now);
  bool Prepare(std::uint64_t token, std::uint64_t id,
               std::shared_ptr<ChannelMovePrepared> prepared, Time now);
  bool Signal(std::uint64_t token, ChannelMoveSignal signal, Time now);
  void Cancel(std::uint64_t token, bool disconnect) noexcept;
  std::optional<ChannelMoveOutcome> Take(std::uint64_t token, Time now);
  static bool Commit(Operation& operation);
  std::shared_ptr<parties::PartyRegistry> parties_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_ = 1;
  std::map<std::uint64_t, Connection> connections_;
};
}  // namespace bubble_fighter::server::features::channels
#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHANNELS_PARTY_CHANNEL_TRANSFERS_H_
