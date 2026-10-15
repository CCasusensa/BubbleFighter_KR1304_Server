#include "application/server_application.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include "application/account_command.h"
#include "features/authentication/account_authentication.h"
#include "features/rooms/match_profile_updates.h"
#include "features/shop/shop_catalog_response.h"
#include "network/clan_tcp_server.h"
#include "network/connection_heartbeat.h"
#include "network/tcp_server.h"
#include "persistence/my_sql_account_store.h"
#include "persistence/my_sql_character_store.h"
#include "persistence/my_sql_clan_store.h"
#include "persistence/my_sql_gift_store.h"
#include "persistence/my_sql_inventory_store.h"
#include "persistence/my_sql_match_store.h"
#include "persistence/my_sql_social_store.h"
#include "sessions/clan_session.h"
#include "sessions/connection_handoff.h"
#include "sessions/menu_snapshot.h"
#include "sessions/player_session.h"

namespace bubble_fighter::server::application {
namespace {
std::uint64_t UnixSeconds() {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  if (seconds <= 0) throw std::runtime_error("Invalid clan maintenance time.");
  return static_cast<std::uint64_t>(seconds);
}
void PrintUsage() {
  std::cout
      << "Status: local ticket authentication and character persistence implemented; full game login pending.\n"
      << "Usage: BubbleFighter.Server --listen-port <0..65535> [--listen-address <IPv4>] [--once] [--database-config <path>] [--idle-timeout-seconds <1..600>] [--match-rules <path>]\n"
      << "Default listen address: 127.0.0.1. Use a local IPv4 address for LAN or 0.0.0.0 for all IPv4 interfaces. Port 0 selects an available port.\n";
  std::cout
      << "Accounts: --create-account <username> --database-config <path>\n"
      << "Login: --login <username> --database-config <path> --ticket-file <new-path>\n"
      << "Optional channels: --channel-rules <path> enables isolated categories with explicit level limits.\n"
      << "Optional clan service: --clan-port <0..65535> requires --database-config and the clan migration.\n"
      << "Clan schedule: --clan-maintenance-unix <future UTC Unix seconds> with --clan-port.\n"
      << "Offline maintenance: --clan-maintenance --database-config <path> (stop all game services first).\n"
      << "Passwords are read from UTF-8 standard input with console echo disabled.\n";
}
}  // namespace

int ServerApplication::Run(int argc, char* argv[]) const {
  std::cout << "BubbleFighter.Server\n";
  if (AccountCommand::IsRequested(argc, argv)) {
    return AccountCommand{}.Run(argc, argv);
  }
  if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
    PrintUsage();
    return 0;
  }

  bool maintenance_only = false;
  std::uint64_t maintenance_at = 0;
  bool has_maintenance = false;
  bool has_port = false;
  bool has_listen_address = false;
  std::string_view listen_address = "127.0.0.1";
  bool once = false;
  unsigned int port = 0;
  bool has_clan_port = false;
  unsigned int clan_port = 0;
  unsigned int idle_timeout_seconds = 30;
  bool has_idle_timeout = false;
  std::filesystem::path database_configuration;
  std::filesystem::path match_rules_path;
  std::filesystem::path channel_rules_path;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--clan-maintenance" && !maintenance_only) {
      maintenance_only = true;
    } else if (argument == "--clan-maintenance-unix" && !has_maintenance &&
               index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto result = std::from_chars(
          value.data(), value.data() + value.size(), maintenance_at);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || !maintenance_at ||
          maintenance_at >= 4102444800ULL) {
        std::cerr << "Invalid clan maintenance time.\n";
        return 2;
      }
      has_maintenance = true;
    } else if (argument == "--listen-port" && !has_port && index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto result =
          std::from_chars(value.data(), value.data() + value.size(), port);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || port > 65535) {
        std::cerr << "Invalid listen port.\n";
        return 2;
      }
      has_port = true;
    } else if (argument == "--clan-port" && !has_clan_port &&
               index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto result =
          std::from_chars(value.data(), value.data() + value.size(), clan_port);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || clan_port > 65535) {
        std::cerr << "Invalid clan port.\n";
        return 2;
      }
      has_clan_port = true;
    } else if (argument == "--listen-address" && !has_listen_address &&
               index + 1 < argc) {
      listen_address = argv[++index];
      try {
        network::TcpServer::ParseListenAddress(listen_address);
      } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
      }
      has_listen_address = true;
    } else if (argument == "--database-config" &&
               database_configuration.empty() && index + 1 < argc) {
      database_configuration = argv[++index];
    } else if (argument == "--idle-timeout-seconds" && !has_idle_timeout &&
               index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto result = std::from_chars(
          value.data(), value.data() + value.size(), idle_timeout_seconds);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() ||
          idle_timeout_seconds == 0 || idle_timeout_seconds > 600) {
        std::cerr << "Invalid idle timeout.\n";
        return 2;
      }
      has_idle_timeout = true;
    } else if (argument == "--match-rules" && match_rules_path.empty() &&
               index + 1 < argc) {
      match_rules_path = argv[++index];
    } else if (argument == "--channel-rules" && channel_rules_path.empty() &&
               index + 1 < argc) {
      channel_rules_path = argv[++index];
    } else if (argument == "--once" && !once) {
      once = true;
    } else {
      std::cerr << "Invalid or duplicate argument: " << argument << '\n';
      PrintUsage();
      return 2;
    }
  }
  if (maintenance_only) {
    if (argc != 4 || database_configuration.empty() || has_port ||
        has_clan_port || has_maintenance) {
      std::cerr << "Maintenance requires only --database-config.\n";
      return 2;
    }
    try {
      persistence::MySqlClanStore maintenance(database_configuration);
      maintenance.LockService();
      const auto count = maintenance.DisbandDue(UnixSeconds());
      std::cout << "Clans disbanded: " << count << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (has_maintenance && (!has_clan_port || maintenance_at <= UnixSeconds())) {
    std::cerr
        << "Clan schedule requires a future maintenance time and clan service.\n";
    return 2;
  }
  if (!has_port) {
    std::cerr << "--listen-port is required to start listening.\n";
    return 2;
  }
  if (has_clan_port &&
      (database_configuration.empty() || (port && port == clan_port))) {
    std::cerr
        << "Clan service requires a database and a distinct listener port.\n";
    return 2;
  }
  try {
    std::shared_ptr<features::clans::ClanAuthentication> clan_authentication;
    std::unique_ptr<persistence::MySqlClanStore> clan_service_guard;
    if (has_clan_port) {
      clan_service_guard =
          std::make_unique<persistence::MySqlClanStore>(database_configuration);
      clan_service_guard->LockService();
      clan_authentication =
          std::make_shared<features::clans::ClanAuthentication>();
    }
    const auto rules =
        match_rules_path.empty()
            ? features::rooms::MatchRewardPolicy{}
            : features::rooms::MatchRewardPolicy::Load(match_rules_path);
    const auto channel_rules =
        channel_rules_path.empty()
            ? features::channels::ChannelRules{}
            : features::channels::ChannelRules::Load(channel_rules_path);
    features::rooms::RoomMatchSettler settler;
    std::shared_ptr<features::social::SocialRegistry> social;
    if (!database_configuration.empty()) {
      // Fail before listening if the required receipt migration is absent.
      persistence::MySqlMatchStore check_schema(database_configuration);
      persistence::MySqlInventoryStore check_inventory(database_configuration);
      persistence::MySqlSocialStore check_social(database_configuration);
      persistence::MySqlGiftStore check_gifts(database_configuration);
      social = std::make_shared<features::social::SocialRegistry>(
          [database_configuration] {
            return std::make_unique<persistence::MySqlSocialStore>(
                database_configuration);
          });
      settler = [database_configuration](
                    const features::rooms::RoomMatchRequest& request) {
        return persistence::MySqlMatchStore(database_configuration)
            .Settle(request);
      };
    }
    const auto parties = std::make_shared<features::parties::PartyRegistry>(
        8, [social](std::uint64_t sender, std::uint64_t recipient) {
          return !social || (social->AllowsPublicChat(sender, recipient) &&
                             social->AllowsPublicChat(recipient, sender));
        });
    std::shared_ptr<features::clans::ClanChannelAccess> clan_channel_access;
    std::shared_ptr<features::clans::ClanPlazaAccess> clan_plaza_access;
    if (clan_authentication) {
      clan_channel_access =
          std::make_shared<features::clans::ClanChannelAccess>(
              clan_authentication,
              [database_configuration](const auto identities) {
                return persistence::MySqlClanStore(database_configuration)
                    .ReadChannelMembers(identities);
              });
      clan_plaza_access = std::make_shared<features::clans::ClanPlazaAccess>(
          clan_authentication, [database_configuration](const auto identities) {
            return persistence::MySqlClanStore(database_configuration)
                .ReadPlazaMembers(identities);
          });
    }
    const auto channels = std::make_shared<features::channels::ChannelWorlds>(
        channel_rules, parties,
        [social](std::uint64_t sender, std::uint64_t recipient) {
          return !social || (social->AllowsPublicChat(sender, recipient) &&
                             social->AllowsPublicChat(recipient, sender));
        },
        std::move(settler), rules, std::move(clan_channel_access),
        std::move(clan_plaza_access));
    const auto initial_world = channels->Find(60);
    const auto rooms = initial_world->rooms;
    const auto plazas = initial_world->plazas;
    const auto invitations = initial_world->invitations;
    const auto scene_transfers = initial_world->transfers;
    // Match rewards outlive individual TCP handlers and even the last
    // room member. The worker holds no room-table lock during retries.
    std::jthread settlement_worker;
    if (!database_configuration.empty()) {
      settlement_worker =
          std::jthread([rooms, database_configuration](std::stop_token stop) {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            std::unique_lock wait_lock(wait_mutex);
            std::array<std::uint8_t, 16> cursor{};
            auto recover_at = std::chrono::steady_clock::time_point{};
            bool recovery_delayed = false;
            while (!stop.stop_requested()) {
              const auto now = std::chrono::steady_clock::now();
              if (now >= recover_at) {
                try {
                  auto pending =
                      persistence::MySqlMatchStore(database_configuration)
                          .LoadPending(cursor);
                  for (auto& request : pending) {
                    rooms->RecoverSettlement(request);
                  }
                  cursor = pending.empty() ? std::array<std::uint8_t, 16>{}
                                           : pending.back().match_id;
                  recovery_delayed = false;
                } catch (const std::exception&) {
                  if (!recovery_delayed) {
                    std::cerr << "Pending match recovery scan delayed.\n";
                  }
                  recovery_delayed = true;
                }
                recover_at = now + std::chrono::seconds{2};
              }
              try {
                rooms->RetrySettlements();
              } catch (const std::exception&) {
                std::cerr << "Cannot poll pending match settlements.\n";
              }
              wake.wait_for(wait_lock, stop, std::chrono::milliseconds{100},
                            [] { return false; });
            }
          });
    }
    const auto handoffs = std::make_shared<sessions::ConnectionHandoffs>();
    std::stop_source service_stop;
    std::exception_ptr clan_failure;
    std::jthread clan_worker;
    if (clan_authentication) {
      std::promise<void> ready;
      auto ready_future = ready.get_future();
      clan_worker = std::jthread([clan_authentication, database_configuration,
                                  clan_port, maintenance_at,
                                  idle_timeout_seconds, listen_address,
                                  ready = std::move(ready), &service_stop,
                                  &clan_failure](std::stop_token stop) mutable {
        bool listening = false;
        try {
          const auto profiles =
              [database_configuration](
                  const features::clans::ClanIdentity& identity) {
                return persistence::MySqlClanStore(database_configuration)
                    .Load(identity);
              };
          const auto master_transfers =
              std::make_shared<features::clans::ClanMasterTransfers>(
                  clan_authentication,
                  [database_configuration](const auto& requester,
                                           std::uint32_t target) {
                    return persistence::MySqlClanStore(database_configuration)
                        .PrepareMasterTransfer(requester, target);
                  },
                  [database_configuration](const auto& parties,
                                           const auto& authorized) {
                    return persistence::MySqlClanStore(database_configuration)
                        .CommitMasterTransfer(parties, authorized);
                  },
                  profiles);
          network::ClanTcpServer{}.Run(
              static_cast<std::uint16_t>(clan_port),
              [clan_authentication, database_configuration, profiles,
               master_transfers, maintenance_at] {
                const auto session = std::make_shared<sessions::ClanSession>(
                    clan_authentication, profiles,
                    features::clans::ClanAuthentication::Clock::now,
                    [database_configuration](const auto& identity,
                                             const auto& request, auto observed,
                                             const auto& authorized) {
                      return persistence::MySqlClanStore(database_configuration)
                          .Create(identity, request, observed, authorized);
                    },
                    [database_configuration](const auto& identity,
                                             const auto& request) {
                      return persistence::MySqlClanStore(database_configuration)
                          .Search(identity, request);
                    },
                    [database_configuration](const auto& identity, auto kind) {
                      return persistence::MySqlClanStore(database_configuration)
                          .ReadRoster(identity, kind);
                    },
                    [database_configuration](const auto& identity,
                                             const auto& request,
                                             const auto& authorized) {
                      return persistence::MySqlClanStore(database_configuration)
                          .Mutate(identity, request, authorized);
                    },
                    master_transfers,
                    [database_configuration, maintenance_at](
                        const auto& identity, const auto& request,
                        const auto& authorized) {
                      return persistence::MySqlClanStore(database_configuration)
                          .Disband(identity, request, UnixSeconds(),
                                   maintenance_at, authorized);
                    },
                    [database_configuration](const auto& identity, auto action,
                                             const auto& authorized) {
                      return persistence::MySqlClanStore(database_configuration)
                          .Training(identity, action, UnixSeconds(),
                                    authorized);
                    });
                return network::ClanConnectionHandlers{
                    [session](const protocol::ClanPacket& packet) {
                      return session->Process(packet);
                    },
                    [session] { return session->Poll(); }};
              },
              idle_timeout_seconds,
              [&](std::uint16_t) {
                listening = true;
                ready.set_value();
              },
              stop, listen_address);
        } catch (...) {
          clan_failure = std::current_exception();
          if (!listening) ready.set_exception(clan_failure);
          service_stop.request_stop();
        }
      });
      ready_future
          .get();  // Main listener starts only after clan bind succeeds.
    }
    const auto create_handler =
        [database_configuration, idle_timeout_seconds, rooms, rules, plazas,
         social, invitations, parties, scene_transfers, channels, handoffs,
         clan_authentication]() -> network::ConnectionHandlers {
      sessions::PlayerSession::TicketConsumer consume;
      if (!database_configuration.empty()) {
        consume = [database_configuration](std::string_view ticket) {
          persistence::MySqlAccountStore store(database_configuration);
          return features::authentication::AccountAuthentication{store}
              .ConsumeTicket(ticket);
        };
      }
      std::shared_ptr<features::characters::CharacterStore> characters;
      if (!database_configuration.empty()) {
        characters = std::make_shared<persistence::MySqlCharacterStore>(
            database_configuration);
      }
      sessions::PlayerSession::MenuDataProvider menu;
      sessions::PlayerSession::EquipmentChangeHandler equipment_change;
      sessions::PlayerSession::ShopCatalogProvider shop_catalog;
      sessions::PlayerSession::ShopPurchaseHandler shop_purchase;
      sessions::PlayerSession::CashPickupHandler cash_pickup;
      sessions::PlayerSession::CashRefundHandler cash_refund;
      sessions::PlayerSession::GiftHandler gifts;
      sessions::PlayerSession::GiftCountSource gift_count;
      sessions::PlayerSession::WeaponPresetHandler weapon_preset;
      sessions::PlayerSession::ItemUseHandler item_use;
      if (!database_configuration.empty()) {
        menu = [database_configuration](
                   const features::characters::CharacterRecord& character,
                   std::uint64_t observed_at) {
          persistence::MySqlInventoryStore store(database_configuration);
          auto snapshot = store.LoadMenuSnapshot(character, observed_at);
          if (!snapshot) {
            throw std::runtime_error("Character inventory is unavailable.");
          }
          auto presets = snapshot->presets.CreateNotification(
              character, snapshot->inventory, observed_at);
          auto data = sessions::CreateMenuSnapshot(
              character, std::move(snapshot->inventory), observed_at);
          data.presets = std::move(presets);
          data.cash = std::move(snapshot->cash);
          return data;
        };
        equipment_change =
            [database_configuration](
                const features::characters::CharacterRecord& character,
                std::uint64_t item_identifier, bool equip,
                std::uint64_t observed_at) {
              persistence::MySqlInventoryStore store(database_configuration);
              return store
                  .ChangeEquipment(character, item_identifier, equip,
                                   observed_at)
                  .packets;
            };
        shop_catalog = features::shop::CreateShopCatalogResponse;
        shop_purchase =
            [database_configuration](
                const features::characters::CharacterRecord& character,
                const protocol::ShopPurchaseRequest& request,
                std::uint64_t observed_at) {
              persistence::MySqlInventoryStore store(database_configuration);
              return store.Purchase(character, request, observed_at);
            };
        cash_pickup =
            [database_configuration](
                const features::characters::CharacterRecord& character,
                const protocol::CashInventoryPickupRequest& request,
                std::uint64_t observed_at) {
              persistence::MySqlInventoryStore store(database_configuration);
              return store.Pickup(character, request, observed_at);
            };
        cash_refund = [database_configuration](const auto& character,
                                               const auto& request,
                                               auto observed_at) {
          return persistence::MySqlInventoryStore(database_configuration)
              .Refund(character, request, observed_at);
        };
        gifts = [database_configuration](const auto& character,
                                         const auto& request, auto observed) {
          return persistence::MySqlGiftStore(database_configuration)
              .Execute(character, request, observed);
        };
        gift_count = [database_configuration](const auto& character,
                                              auto observed) {
          return persistence::MySqlGiftStore(database_configuration)
              .Pending(character, observed);
        };
        weapon_preset =
            [database_configuration](
                const features::characters::CharacterRecord& character,
                const protocol::WeaponPresetChangeRequest& request,
                std::uint64_t observed_at) {
              persistence::MySqlInventoryStore store(database_configuration);
              return store.ChangeWeaponPreset(character, request, observed_at)
                  .packets;
            };
        item_use = [database_configuration](
                       const features::characters::CharacterRecord& character,
                       const protocol::ItemUseRequest& request,
                       std::uint64_t observed_at) {
          persistence::MySqlInventoryStore store(database_configuration);
          return store.UseItem(character, request, observed_at);
        };
      }
      features::clans::ClanProfileSource party_clan;
      if (clan_authentication) {
        party_clan = [database_configuration](const auto& identity) {
          return persistence::MySqlClanStore(database_configuration)
              .Load(identity);
        };
      }
      const auto owned_session = std::make_shared<sessions::PlayerSession>(
          std::move(consume), std::move(characters), std::move(menu),
          std::move(equipment_change), std::move(shop_catalog),
          std::move(shop_purchase), std::move(cash_pickup),
          std::move(weapon_preset), std::move(item_use),
          features::practice::PracticeSession::Clock::now,
          features::practice::PracticeSession::CreateRandomSeed, rooms, plazas,
          social, invitations, parties, scene_transfers, channels,
          clan_authentication,
          [database_configuration](const auto& identity, const auto& request,
                                   auto observed) {
            return persistence::MySqlClanStore(database_configuration)
                .ConfirmCreation(identity, request, observed);
          },
          [database_configuration, clan_authentication](const auto& identity) {
            auto operation = clan_authentication->LockOperations();
            auto result =
                persistence::MySqlClanStore(database_configuration)
                    .Training(identity,
                              features::clans::TrainingClanAction::kReconcile,
                              UnixSeconds(), [] { return true; });
            if (!result.mutation.responses.empty()) {
              clan_authentication->InvalidateMembership();
              const std::array deliveries{features::clans::ClanDelivery{
                  {identity.character_id},
                  std::make_shared<const std::vector<protocol::ClanPacket>>(
                      std::move(result.mutation.responses))}};
              clan_authentication->Publish(deliveries);
            }
            return result.graduated;
          },
          std::move(party_clan), std::move(cash_refund), std::move(gifts),
          std::move(gift_count));
      network::ConnectionHandlers handlers;
      handlers.on_packet = [owned_session,
                            rules](const protocol::TcpPacket& packet) {
        auto& session = *owned_session;
        const auto before = session.State();
        auto response = session.Process(packet);
        features::rooms::BindMatchRewardProfiles(rules, response);
        if (before != session.State() &&
            session.State() ==
                sessions::PlayerSessionState::kAwaitingCharacterCreation) {
          std::cout << "Ticket accepted; character creation required."
                    << std::endl;
        }
        if (before != session.State() &&
            session.State() ==
                sessions::PlayerSessionState::kAwaitingCharacterProfile) {
          std::cout
              << (packet.message_id == 3
                      ? "Ticket accepted; stored character profile pending."
                      : "Character persisted; login profile pending.")
              << std::endl;
          if (session.Character()) {
            std::cout << "CharacterType=" << session.Character()->character_type
                      << " ColorIndex=" << session.Character()->color_index
                      << std::endl;
          }
        }
        if (before != session.State() &&
            (session.State() ==
                 sessions::PlayerSessionState::kLoginProfileSent ||
             session.State() ==
                 sessions::PlayerSessionState::kAwaitingMenuInitialization)) {
          std::cout
              << (packet.message_id == 3
                      ? "Ticket accepted; stored character login profile sent."
                      : "Persisted character login profile sent.")
              << std::endl;
        }
        if (before != session.State() &&
            session.State() ==
                sessions::PlayerSessionState::kMenuInitializationSent) {
          std::cout << "Owned character menu initialization sent." << std::endl;
        }
        const bool close =
            session.State() == sessions::PlayerSessionState::kRejected;
        return network::PacketHandlingResult{std::move(response), close};
      };
      handlers.on_tick = [owned_session, rules] {
        auto response = owned_session->Poll();
        features::rooms::BindMatchRewardProfiles(rules, response);
        return network::PacketHandlingResult{std::move(response), false};
      };
      auto group = std::make_shared<sessions::HandoffSessionHooks::Group>();
      group->enable = [owned_session] {
        owned_session->EnableChannelHandoff();
      };
      group->begin = [owned_session](std::uint32_t category) {
        return owned_session->BeginPartyChannelHandoff(category);
      };
      group->poll = [owned_session] {
        return owned_session->PollPartyChannelHandoff();
      };
      group->signal =
          [owned_session](features::channels::ChannelMoveSignal signal) {
            return owned_session->SignalPartyChannelHandoff(signal);
          };
      group->cancel = [owned_session] {
        owned_session->CancelPartyChannelHandoff();
      };
      group->irreversible = [owned_session] {
        return owned_session->PartyChannelIrreversible();
      };
      auto bound = handoffs->Attach(
          std::move(handlers),
          {[owned_session]() -> std::optional<sessions::HandoffIdentity> {
             if (!owned_session->AccountId()) return std::nullopt;
             return sessions::HandoffIdentity{
                 *owned_session->AccountId(),
                 owned_session->Character()
                     ? static_cast<std::uint32_t>(
                           owned_session->Character()->character_id)
                     : 0};
           },
           [owned_session](std::uint32_t category) {
             return owned_session->BeginConnectionHandoff(category);
           },
           [owned_session] { owned_session->EndConnectionHandoff(); },
           [owned_session] {
             return owned_session->CompleteConnectionHandoff();
           },
           std::move(group),
           [owned_session] { return owned_session->ChannelRefusal(); }});
      return network::ConnectionHeartbeat::Wrap(std::move(bound.handlers),
                                                std::move(bound.authenticated),
                                                idle_timeout_seconds);
    };
    const auto result = network::TcpServer{}.Run(
        static_cast<std::uint16_t>(port), once, create_handler,
        idle_timeout_seconds,
        [handoffs, listen_address](std::uint16_t actual_port) {
          const auto address =
              network::TcpServer::ParseListenAddress(listen_address);
          handoffs->SetEndpoint({static_cast<std::uint8_t>(address >> 24),
                                 static_cast<std::uint8_t>(address >> 16),
                                 static_cast<std::uint8_t>(address >> 8),
                                 static_cast<std::uint8_t>(address)},
                                actual_port);
        },
        service_stop.get_token(), listen_address);
    if (clan_worker.joinable()) {
      clan_worker.request_stop();
      clan_worker.join();
    }
    if (clan_failure) std::rethrow_exception(clan_failure);
    return result;
  } catch (const std::exception& error) {
    std::cerr << "Server error: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace bubble_fighter::server::application
