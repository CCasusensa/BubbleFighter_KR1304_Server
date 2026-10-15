#ifndef BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAZA_REGISTRY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAZA_REGISTRY_H_
#include <map>
#include <mutex>

#include "features/chat/scene_chat.h"
#include "features/clans/clan_plaza_access.h"
#include "features/rooms/room_packet_queue.h"
#include "features/scenes/scene_roster.h"
#include "protocol/scenes/plaza_movement.h"

namespace bubble_fighter::server::features::lobby {
class PlazaRegistry;
struct PlazaTarget {
  std::uint16_t scene = 0;
  std::uint64_t account = 0;
  std::uint64_t token = 0;
  std::uint32_t clan_id = 0;
};
// Prepared by the owning authenticated session. The source token is zero only
// when that connection has no plaza membership. These are not wire fields.
struct PlazaEntry {
  scenes::SceneSlot player;
  std::uint64_t account = 0;
  rooms::RoomPacketSink sink;
  std::uint64_t source_token = 0;
};
class PlazaLease {
 public:
  PlazaLease() = default;
  PlazaLease(PlazaLease&& other) noexcept;
  PlazaLease& operator=(PlazaLease&& other) noexcept;
  PlazaLease(const PlazaLease&) = delete;
  PlazaLease& operator=(const PlazaLease&) = delete;
  ~PlazaLease();
  explicit operator bool() const noexcept;
  std::uint64_t Token() const noexcept;
  std::uint32_t ClanId() const noexcept;
  void Release() noexcept;
  void Move(const protocol::PlazaMovementRequest& movement);
  void Refresh(scenes::SceneSlot player, bool send_snapshot);
  void Chat(const features::chat::SceneChat::Message& request);

 private:
  friend class PlazaRegistry;
  PlazaLease(std::shared_ptr<PlazaRegistry> registry, std::uint16_t scene,
             std::uint64_t account, std::uint64_t token,
             std::uint32_t clan_id) noexcept;
  std::shared_ptr<PlazaRegistry> registry_;
  std::uint16_t scene_ = 0;
  std::uint64_t account_ = 0;
  std::uint64_t token_ = 0;
  std::uint32_t clan_id_ = 0;
};

enum class PlazaAdmissionStatus { kEntered, kFull, kStale };
struct PlazaAdmission {
  PlazaAdmissionStatus status = PlazaAdmissionStatus::kStale;
  std::vector<PlazaLease> members;
};

// One registry for the server's current local channel. First non-full
// plaza, lowest vacant slot; capacity 16 is the user's local policy.
// Ordered delivery under the lock uses enqueue-only, nonblocking sinks.
class PlazaRegistry : public std::enable_shared_from_this<PlazaRegistry> {
 public:
  static constexpr std::size_t kCapacity = 16;
  PlazaLease Enter(protocol::GotoRoomNotification envelope,
                   scenes::SceneSlot player, std::uint64_t account,
                   rooms::RoomPacketSink sink);
  std::optional<PlazaTarget> Capture(std::uint64_t account) const;
  // All entries move to one scene or none move. Callers must separately verify
  // party leadership or invitation consent. Target capture is not permission.
  // Queued S117/peer updates are encoded before any membership changes.
  PlazaAdmission EnterGroup(protocol::GotoRoomNotification envelope,
                            std::vector<PlazaEntry> entries,
                            std::optional<PlazaTarget> target = {},
                            clans::ClanPlazaScope scope = {});

 private:
  friend class PlazaLease;
  struct Member {
    std::uint32_t slot = 0;
    scenes::SceneSlot player;
    rooms::RoomPacketSink sink;
    std::optional<protocol::PlazaMovementRequest> movement;
    std::uint64_t token = 0;
  };
  struct Scene {
    protocol::GotoRoomNotification envelope;
    std::map<std::uint64_t, Member> members;
    clans::ClanPlazaScope scope;
  };
  static protocol::GotoRoomNotification Snapshot(const Scene& scene,
                                                 std::uint64_t account);
  static protocol::TcpPacket Update(
      const protocol::GotoRoomNotification& snapshot);
  static void Deliver(const Member& member,
                      std::vector<protocol::TcpPacket> packets) noexcept;
  void Leave(std::uint16_t scene, std::uint64_t account,
             std::uint64_t token) noexcept;
  void Move(std::uint16_t scene, std::uint64_t account, std::uint64_t token,
            const protocol::PlazaMovementRequest& movement);
  void Refresh(std::uint16_t scene, std::uint64_t account, std::uint64_t token,
               scenes::SceneSlot player, bool send_snapshot);
  void Chat(std::uint16_t scene, std::uint64_t account, std::uint64_t token,
            const features::chat::SceneChat::Message& request);
  std::uint64_t NextToken();
  bool Contains(std::uint16_t scene, std::uint64_t account,
                std::uint64_t token) const;
  mutable std::mutex mutex_;
  std::uint64_t next_token_ = 1;
  std::map<std::uint16_t, Scene> scenes_;
};
}  // namespace bubble_fighter::server::features::lobby

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAZA_REGISTRY_H_
