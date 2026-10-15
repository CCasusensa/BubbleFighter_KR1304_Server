#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_PLAYER_SESSION_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_PLAYER_SESSION_H_

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "features/authentication/initial_handshake.h"
#include "features/channels/channel_worlds.h"
#include "features/characters/character_store.h"
#include "features/chat/scene_chat.h"
#include "features/clans/clan_authentication.h"
#include "features/clans/clan_creation.h"
#include "features/clans/clan_profile.h"
#include "features/clans/training_clan.h"
#include "features/gifts/gift_service.h"
#include "features/inventory/item_use.h"
#include "features/lobby/lobby_session.h"
#include "features/lobby/plaza_registry.h"
#include "features/parties/party_registry.h"
#include "features/parties/party_scene_transfers.h"
#include "features/practice/practice_session.h"
#include "features/rooms/room_packet_queue.h"
#include "features/shop/cash_refund.h"
#include "features/shop/cash_shop.h"
#include "features/shop/shop_purchase.h"
#include "features/social/room_invitations.h"
#include "features/social/social_registry.h"
#include "protocol/inventory/weapon_preset_change_request.h"
#include "protocol/shop/shop_entry_request.h"
#include "protocol/shop/shop_entry_response.h"
#include "sessions/menu_initialization.h"

namespace bubble_fighter::server::sessions {
enum class PlayerSessionState {
  kAwaitingInitialRecord,
  kAwaitingTicket,
  kAwaitingCharacterCreation,
  kAwaitingCharacterProfile,
  kLoginProfileSent,
  kAwaitingMenuInitialization,
  kMenuInitializationSent,
  kPracticeLoading,
  kPracticeCountdown,
  kPracticePlaying,
  kPracticeAwaitingReport,
  kPracticeFinishing,
  kPracticeResult,
  kInRoom,
  kRejected
};

class PlayerSession {
 public:
  using TicketConsumer =
      std::function<std::optional<std::uint64_t>(std::string_view)>;
  // Supplies trusted local initial state including loaded inventory.
  // ServerApplication installs CreateMenuSnapshot for configured MySQL;
  // callers without a provider retain the explicitly partial legacy path.
  // One observation time is shared by persistence, view and packet encoding.
  using MenuDataProvider = std::function<MenuInitializationData(
      const features::characters::CharacterRecord&,
      std::uint64_t observed_at_unix_seconds)>;
  using EquipmentChangeHandler = std::function<std::vector<protocol::TcpPacket>(
      const features::characters::CharacterRecord&,
      std::uint64_t item_identifier, bool equip,
      std::uint64_t observed_at_unix_seconds)>;
  using ShopCatalogProvider = std::function<protocol::ShopEntryResponse(
      const protocol::ShopEntryRequest&,
      const features::characters::CharacterRecord&,
      std::uint64_t observed_at_unix_seconds)>;
  using ShopPurchaseHandler = std::function<features::shop::ShopPurchaseResult(
      const features::characters::CharacterRecord&,
      const protocol::ShopPurchaseRequest&,
      std::uint64_t observed_at_unix_seconds)>;
  using CashRefundHandler = std::function<features::shop::CashRefundResult(
      const features::characters::CharacterRecord&,
      const protocol::CashRefundRequest&,
      std::uint64_t observed_at_unix_seconds)>;
  using GiftHandler = std::function<features::gifts::GiftResult(
      const features::characters::CharacterRecord&,
      const features::gifts::GiftRequest&, std::uint64_t)>;
  using GiftCountSource = std::function<std::uint32_t(
      const features::characters::CharacterRecord&, std::uint64_t)>;
  using CashPickupHandler = std::function<features::shop::CashPickupResult(
      const features::characters::CharacterRecord&,
      const protocol::CashInventoryPickupRequest&,
      std::uint64_t observed_at_unix_seconds)>;
  using WeaponPresetHandler = std::function<std::vector<protocol::TcpPacket>(
      const features::characters::CharacterRecord&,
      const protocol::WeaponPresetChangeRequest&,
      std::uint64_t observed_at_unix_seconds)>;
  using ItemUseHandler = std::function<features::inventory::ItemUseResult(
      const features::characters::CharacterRecord&,
      const protocol::ItemUseRequest&, std::uint64_t observed_at_unix_seconds)>;
  using PracticeClock =
      std::function<features::practice::PracticeSession::Time()>;
  using PracticeSeedFactory = std::function<std::uint32_t()>;
  explicit PlayerSession(
      TicketConsumer consume_ticket = {},
      std::shared_ptr<features::characters::CharacterStore> characters = {},
      MenuDataProvider menu_data = {},
      EquipmentChangeHandler equipment_change = {},
      ShopCatalogProvider shop_catalog = {},
      ShopPurchaseHandler shop_purchase = {},
      CashPickupHandler cash_pickup = {},
      WeaponPresetHandler weapon_preset = {}, ItemUseHandler item_use = {},
      PracticeClock practice_clock =
          features::practice::PracticeSession::Clock::now,
      PracticeSeedFactory practice_seed =
          features::practice::PracticeSession::CreateRandomSeed,
      std::shared_ptr<features::rooms::RoomRegistry> rooms = {},
      std::shared_ptr<features::lobby::PlazaRegistry> plazas = {},
      std::shared_ptr<features::social::SocialRegistry> social = {},
      std::shared_ptr<features::social::RoomInvitations> invitations = {},
      std::shared_ptr<features::parties::PartyRegistry> parties = {},
      std::shared_ptr<features::parties::PartySceneTransfers> scene_transfers =
          {},
      std::shared_ptr<features::channels::ChannelWorlds> channels = {},
      std::shared_ptr<features::clans::ClanAuthentication> clan_authentication =
          {},
      features::clans::ClanConfirmationHandler clan_confirmation = {},
      features::clans::TrainingClanReconciler training = {},
      features::clans::ClanProfileSource party_clan = {},
      CashRefundHandler cash_refund = {}, GiftHandler gifts = {},
      GiftCountSource gift_count = {});
  std::vector<protocol::TcpPacket> Process(const protocol::TcpPacket& packet);
  // Called by the transport even when no packet arrives. Uses monotonic time.
  std::vector<protocol::TcpPacket> Poll();
  PlayerSessionState State() const noexcept;
  std::optional<std::uint64_t> AccountId() const noexcept;
  const std::optional<features::characters::CharacterRecord>& Character()
      const noexcept;
  bool BeginConnectionHandoff(std::uint32_t category = 60);
  std::vector<protocol::TcpPacket> CompleteConnectionHandoff();
  std::uint32_t ChannelCategory() const noexcept;
  std::uint32_t ChannelRefusal() const noexcept;
  void EndConnectionHandoff();
  void EnableChannelHandoff();
  std::optional<bool> BeginPartyChannelHandoff(std::uint32_t category);
  features::channels::ChannelMoveStatus PollPartyChannelHandoff();
  bool SignalPartyChannelHandoff(features::channels::ChannelMoveSignal signal);
  void CancelPartyChannelHandoff() noexcept;
  bool PartyChannelIrreversible() const;

 private:
  std::vector<protocol::TcpPacket> DrainRoomPackets();
  std::optional<std::vector<protocol::TcpPacket>> ProcessGifts(
      const protocol::TcpPacket& packet);
  std::vector<protocol::TcpPacket> PollGifts();
  std::optional<std::vector<protocol::TcpPacket>> ProcessClan(
      const protocol::TcpPacket& packet);
  static std::uint64_t InventoryObservationTime();
  std::optional<std::vector<protocol::TcpPacket>> ProcessCommerce(
      const protocol::TcpPacket& packet);
  std::optional<std::vector<protocol::TcpPacket>> ProcessPractice(
      const protocol::TcpPacket& packet);
  std::vector<protocol::TcpPacket> ProcessCore(
      const protocol::TcpPacket& packet);
  std::optional<std::vector<protocol::TcpPacket>> ProcessRoom(
      const protocol::TcpPacket& packet);
  std::optional<std::vector<protocol::TcpPacket>> ProcessSocial(
      const protocol::TcpPacket& packet);
  std::optional<std::vector<protocol::TcpPacket>> ProcessChannel(
      const protocol::TcpPacket& packet);
  std::optional<std::vector<protocol::TcpPacket>> ProcessInvitation(
      const protocol::TcpPacket& packet);
  void SyncInvitations();
  void SyncParty();
  void RefreshPartyClan();
  void CheckClanChannel();
  void CheckClanPlaza();
  std::vector<protocol::TcpPacket> PollPartyScene();
  std::optional<std::vector<protocol::TcpPacket>> ProcessPartyScene(
      const protocol::TcpPacket& packet);
  MenuInitializationData LoadMenuData(std::uint64_t observed_at);
  void BeginSceneChange();
  void SyncClanScene();
  std::vector<protocol::TcpPacket> PollTrainingClan();
  void InvalidateClanScene();
  std::optional<std::vector<protocol::TcpPacket>> ProcessParty(
      const protocol::TcpPacket& packet);
  std::vector<protocol::TcpPacket> SendLoginProfile();
  std::vector<protocol::TcpPacket> ReturnFromPractice();
  std::vector<protocol::TcpPacket> EnterPlaza(MenuInitializationData data);
  void Reject() noexcept;
  void UpdatePracticeState() noexcept;
  bool CommerceInitialized() const noexcept;
  std::vector<protocol::TcpPacket> ChangeInventory(
      const std::function<std::vector<protocol::TcpPacket>()>& change);
  features::chat::SceneChat chat_;
  std::shared_ptr<features::clans::ClanAuthentication> clan_authentication_;
  features::clans::ClanAuthentication::GameLink clan_game_;
  features::clans::ClanConfirmationHandler clan_confirmation_;
  features::clans::TrainingClanReconciler training_;
  features::clans::ClanProfileSource party_clan_;
  std::uint32_t party_clan_id_ = 0;
  std::uint64_t party_clan_revision_ = 0;
  std::optional<features::practice::PracticeSession::Time> party_clan_poll_at_;
  std::optional<features::practice::PracticeSession::Time> training_poll_at_;
  bool training_graduation_sent_ = false;
  bool connection_handoff_ = false;
  struct ChannelTransfer;
  std::shared_ptr<ChannelTransfer> PrepareChannelDestination(
      std::shared_ptr<const features::channels::ChannelWorld> destination);
  std::shared_ptr<ChannelTransfer> channel_transfer_;
  std::shared_ptr<features::channels::ChannelWorlds> channels_;
  std::uint32_t channel_category_ = 60;
  std::uint32_t channel_refusal_ = 2;
  std::uint64_t channel_party_id_ = 0, channel_party_epoch_ = 0,
                channel_revision_ = 0;
  std::vector<features::clans::ClanIdentity> channel_members_;
  std::optional<features::practice::PracticeSession::Time> channel_poll_at_;
  std::uint32_t plaza_clan_id_ = 0;
  std::uint64_t plaza_party_id_ = 0, plaza_party_epoch_ = 0,
                plaza_clan_revision_ = 0;
  std::vector<features::clans::ClanIdentity> plaza_clan_members_;
  std::optional<features::practice::PracticeSession::Time> plaza_clan_poll_at_;
  features::authentication::InitialHandshake handshake_;
  TicketConsumer consume_ticket_;
  std::shared_ptr<features::characters::CharacterStore> characters_;
  MenuDataProvider menu_data_;
  EquipmentChangeHandler equipment_change_;
  ShopCatalogProvider shop_catalog_;
  ShopPurchaseHandler shop_purchase_;
  CashPickupHandler cash_pickup_;
  CashRefundHandler cash_refund_;
  GiftHandler gifts_;
  GiftCountSource gift_count_;
  std::optional<features::practice::PracticeSession::Time> gift_poll_at_;
  std::optional<std::uint32_t> gift_pending_;
  WeaponPresetHandler weapon_preset_;
  ItemUseHandler item_use_;
  std::optional<std::uint64_t> shop_revision_sent_;
  PlayerSessionState state_ = PlayerSessionState::kAwaitingInitialRecord;
  std::optional<std::uint64_t> account_id_;
  std::optional<features::characters::CharacterRecord> character_;
  std::u16string checked_name_;
  std::optional<features::lobby::LobbySession> lobby_;
  PracticeClock practice_clock_;
  PracticeSeedFactory practice_seed_;
  std::optional<features::practice::PracticeSession> practice_;
  std::shared_ptr<features::rooms::RoomRegistry> rooms_;
  std::shared_ptr<features::rooms::RoomPacketQueue> room_packets_ =
      std::make_shared<features::rooms::RoomPacketQueue>();
  features::rooms::RoomLease room_;
  std::shared_ptr<features::lobby::PlazaRegistry> plazas_;
  std::shared_ptr<features::rooms::RoomPacketQueue> plaza_packets_ =
      std::make_shared<features::rooms::RoomPacketQueue>();
  features::lobby::PlazaLease plaza_;
  std::shared_ptr<features::social::SocialRegistry> social_;
  std::shared_ptr<features::rooms::RoomPacketQueue> social_packets_ =
      std::make_shared<features::rooms::RoomPacketQueue>();
  features::social::SocialLease social_lease_;
  std::shared_ptr<features::social::RoomInvitations> invitations_;
  features::social::InvitationLease invitation_lease_;
  std::shared_ptr<features::parties::PartyRegistry> parties_;
  features::parties::PartyMembership party_membership_;
  std::optional<protocol::PartySupplementaryRecord> party_equipment_;
  std::shared_ptr<features::parties::PartySceneTransfers> scene_transfers_;
  features::parties::PartySceneConnection scene_connection_;
  std::shared_ptr<features::parties::PartyScenePrepared> scene_prepared_;
  features::channels::PartyChannelConnection channel_connection_;
};
}  // namespace bubble_fighter::server::sessions

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_PLAYER_SESSION_H_
