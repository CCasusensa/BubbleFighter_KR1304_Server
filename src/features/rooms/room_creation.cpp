#include "features/rooms/room_creation.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <syncstream>
#include <utility>

#include "features/rooms/room_map_catalog.h"
#include "features/rooms/room_roster.h"
#include "protocol/codec/packet_writer.h"

namespace bubble_fighter::server::features::rooms {
namespace {
template <typename Value>
bool Contains(const std::vector<Value>& values, Value value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void ValidatePolicy(const RoomCreationPolicy& policy) {
  if (std::any_of(policy.playable_modes.begin(), policy.playable_modes.end(),
                  RoomCreation::IsLargeRoomMode) ||
      policy.max_title_code_units >
          protocol::PacketWriter::kMaxStringCodeUnits ||
      policy.max_password_code_units >
          protocol::PacketWriter::kMaxStringCodeUnits) {
    throw std::invalid_argument(
        "Room creation policy exceeds the supported room shapes.");
  }
}

}  // namespace

void RoomCreation::ValidateRequest(const protocol::CreateRoomRequest& request) {
  // 0x009622A3 clears ShortAt0 and no creation path assigns
  // SessionType after 0x0113B7B0 sets it to 0. The server owns both.
  if (request.settings.short_at0 != 0 || request.settings.session_type != 0) {
    throw std::invalid_argument(
        "Create-room request carries a server-owned room identity.");
  }
  // 0x00962272..0x00962284: the flag is exactly "password not empty".
  if (request.settings.boolean_at38 != (request.password.empty() ? 0U : 1U)) {
    throw std::invalid_argument(
        "Create-room password flag does not match the password.");
  }
}

namespace {
RoomGroupCreationOutcome Refuse(protocol::CreateRoomResult result) {
  RoomGroupCreationOutcome outcome;
  outcome.result = result;
  outcome.responses.push_back(
      protocol::CreateRoomResponse{0, static_cast<std::uint32_t>(result), 0}
          .Encode());
  return outcome;
}
}  // namespace

bool RoomCreation::IsLargeRoomMode(std::uint16_t mode) noexcept {
  // 0x01150EF0 returns true for 32, and for these when 0x01150F80 is
  // false. That runtime condition is unknown, so all are treated as large.
  switch (mode) {
    case 0x17:
    case 0x19:
    case 0x1E:
    case 0x20:
    case 0x2A:
    case 0x2C:
    case 0x2D:
      return true;
    default:
      return false;
  }
}

RoomCreationOutcome RoomCreation::Create(
    RoomRegistry& registry, const RoomCreationPolicy& policy,
    const protocol::CreateRoomRequest& request, RoomCreationInput input) {
  std::vector<RoomCreationInput> inputs;
  inputs.push_back(std::move(input));
  auto group = CreateGroup(registry, policy, request, std::move(inputs));
  RoomCreationOutcome outcome;
  outcome.result = group.result;
  outcome.responses = std::move(group.responses);
  if (!group.members.empty()) outcome.room = std::move(group.members.front());
  return outcome;
}

RoomGroupCreationOutcome RoomCreation::CreateGroup(
    RoomRegistry& registry, const RoomCreationPolicy& policy,
    const protocol::CreateRoomRequest& request,
    std::vector<RoomCreationInput> inputs) {
  using protocol::CreateRoomResult;
  ValidatePolicy(policy);
  ValidateRequest(request);
  if (inputs.empty() || inputs.size() > kOrdinaryRoomSlots) {
    throw std::invalid_argument("Unsupported room admission group.");
  }
  std::vector<RoomIdentity> identities;
  for (const auto& input : inputs) {
    const auto& character = input.character;
    if (input.authenticated_account_id == 0 ||
        character.account_id != input.authenticated_account_id ||
        character.character_id == 0 ||
        character.character_id > std::numeric_limits<std::uint32_t>::max() ||
        input.equipment.key != character.character_id || !input.sink) {
      throw std::invalid_argument(
          "Room host does not belong to the authenticated account.");
    }
    identities.push_back(
        {input.authenticated_account_id, character.character_id});
  }

  // The dialog's own checkboxes reach the wire as bits of
  // Configuration.ValueAt0 and as the contest byte at Configuration+8
  // (0x00856060..0x00856146). They are the host's choices and every hosted
  // mode accepts them, so they are normalized and echoed in the room
  // snapshot instead of being refused. 0x00962272 derives BooleanAt38 from
  // the password alone and ValidateRequest already checked it.
  auto settings = request.settings;
  settings.configuration.value_at0 &= kOrdinaryOptionFlags;
  settings.configuration.boolean_at8 &= 0xFFU;
  const auto& configuration = settings.configuration;
  if (IsLargeRoomMode(configuration.short_at4) ||
      !Contains(policy.playable_modes, configuration.short_at4)) {
    std::osyncstream(std::cout)
        << "Room creation refused: mode=" << configuration.short_at4
        << " is not hosted (large room or outside the policy list)."
        << std::endl;
    return Refuse(CreateRoomResult::kModeUnavailable);
  }
  // Unsupported creation variants: a trailing value no dialog produces, a
  // flag bit no checkbox sets, an unknown room kind, and local
  // title/password limits. They are refused rather than reinterpreted.
  if ((request.trailing_value && *request.trailing_value != 0) ||
      (request.settings.configuration.value_at0 & ~kOrdinaryOptionFlags) != 0 ||
      !Contains(policy.room_kinds, configuration.values_at20[0]) ||
      settings.text_at8.empty() ||
      settings.text_at8.size() > policy.max_title_code_units ||
      request.password.size() > policy.max_password_code_units) {
    std::osyncstream(std::cout)
        << "Room creation refused: mode=" << configuration.short_at4
        << " kind=" << configuration.values_at20[0]
        << " flag_at0=" << request.settings.configuration.value_at0
        << " flag_at8=" << request.settings.configuration.boolean_at8
        << " trailing=" << (request.trailing_value ? 1 : 0)
        << " title=" << settings.text_at8.size()
        << " password=" << request.password.size() << std::endl;
    return Refuse(CreateRoomResult::kFailed);
  }

  // Local policy: one room per account/character; a repeated request
  // while a room or claim exists is refused without changing it.
  auto reserved = registry.ReserveGroup(identities);
  if (reserved.status != RoomReserveStatus::kReserved) {
    return Refuse(CreateRoomResult::kFailed);
  }
  auto& reservation = *reserved.reservation;

  // New rooms default to Random; accepted later selections persist.
  // RoomRoster::Describe assigns
  // the server-owned room number (C403 identity, 0x00855310), state,
  // counts and S117 type whenever a descriptor is sent.
  RoomRecord record;
  record.room_id = reservation.RoomId();
  record.slot_count = kOrdinaryRoomSlots;
  // Diagnostic: manual create-room tests use this line to record which wire
  // mode id each dialog page sends (the dialog's ArrayIndex is a UI index).
  std::osyncstream(std::cout)
      << "Room created: mode=" << configuration.short_at4
      << " kind=" << configuration.values_at20[0]
      << " options=" << configuration.value_at0
      << " contest=" << configuration.boolean_at8
      << " slots=" << record.slot_count << std::endl;
  record.host_slot = kHostSlot;
  record.state = RoomState::kWaiting;
  record.settings = settings;
  RoomMapCatalog::DefaultRandom(record.settings.configuration);
  record.password = request.password;
  for (auto& input : inputs) {
    RoomOccupant host;
    host.slot = static_cast<std::uint32_t>(record.occupants.size());
    host.account_id = input.authenticated_account_id;
    host.character = std::move(input.character);
    host.member = std::move(input.member);
    // 96DD20 accepts active teams 1/2; 36 is reserved for spectators.
    host.member.signed_byte_at4 = static_cast<std::uint8_t>(1 + host.slot % 2);
    host.member.booleans_at96 = {};
    host.equipment = std::move(input.equipment);
    record.occupants.push_back(std::move(host));
  }

  // 0x00856F14 marks this client as creator before S117 builds stage 4.
  // Every member gets the final roster; followers are not marked as creators.
  std::vector<RoomPublication> deliveries;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    std::vector<protocol::TcpPacket> packets;
    if (i == 0) packets.push_back(protocol::CreateRoomResponse{}.Encode());
    packets.push_back(
        RoomRoster::Snapshot(record, static_cast<std::uint32_t>(i)).Encode());
    deliveries.push_back({inputs[i].authenticated_account_id,
                          std::move(inputs[i].sink), std::move(packets)});
  }

  RoomGroupCreationOutcome outcome;
  outcome.result = CreateRoomResult::kCreated;
  outcome.members = std::move(reservation)
                        .CommitGroup(std::move(record), std::move(deliveries));
  return outcome;
}
}  // namespace bubble_fighter::server::features::rooms
