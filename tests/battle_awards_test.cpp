#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>

#include "features/rooms/battle_record_awards.h"

namespace bf = bubble_fighter::server;
namespace rooms = bf::features::rooms;
namespace chars = bf::features::characters;
namespace {
int checks = 0;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok)
    throw std::runtime_error("Battle awards check line " +
                             std::to_string(at.line()));
}
template <class F>
void Rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected);
}
rooms::RoomMatchRequest Request(unsigned version = 3) {
  rooms::RoomMatchRequest r;
  r.format_version = version;
  r.match_id = rooms::CreateRoomMatchId();
  r.duration_milliseconds = 240000;
  r.team_scores = {9, 4};
  r.members = {{0, 11, 21, 1, {3, 5, 7, 11}}, {1, 12, 22, 2, {2, 4, 6, 8}}};
  return r;
}
}  // namespace

int RunBattleAwardsTests(const std::filesystem::path& directory) {
  std::ifstream file(directory / "battle_awards_cases.txt");
  Check(file.good());
  std::string line;
  unsigned rows = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream input(line);
    auto r = Request();
    unsigned team = 0, base = 0;
    std::array<unsigned, 6> expected{};
    Check(static_cast<bool>(
        input >> r.format_version >> team >> r.team_scores[0] >>
        r.team_scores[1] >> r.members[0].scores[0] >> r.members[0].scores[1] >>
        r.members[0].scores[2] >> r.members[0].scores[3] >> base));
    for (auto& n : expected) Check(static_cast<bool>(input >> n));
    r.members[0].team = static_cast<std::uint8_t>(team);
    chars::BattleRecords before;
    before.values.fill(base);
    const auto result = rooms::BattleRecordAwards::Apply(r, 0, before);
    for (std::size_t i = 0; i < 43; ++i)
      Check(result.values[i] == (i >= 1 && i <= 6 ? expected[i - 1] : base));
    Check(before.values[1] == base);
    auto bytes = r.Encode();
    Check(rooms::RoomMatchRequest::Decode(bytes).Encode() == bytes);
    Check(bytes[0] == r.format_version && bytes[1] == 0);
    if (r.format_version == 3) {
      auto old = r;
      old.format_version = 2;
      auto old_bytes = old.Encode();
      old_bytes[0] = 3;
      Check(bytes == old_bytes);
      for (std::size_t n = 0; n < bytes.size(); ++n)
        Rejects([&] {
          rooms::RoomMatchRequest::Decode(std::span(bytes).first(n));
        });
      bytes.push_back(0);
      Rejects([&] { rooms::RoomMatchRequest::Decode(bytes); });
    }
    ++rows;
  }
  Check(rows == 12);
  auto r = Request();
  for (auto version : {0u, 4u, 255u}) {
    r.format_version = version;
    Rejects([&] { rooms::BattleRecordAwards::Apply(r, 0, {}); });
  }
  r = Request();
  Rejects([&] { rooms::BattleRecordAwards::Apply(r, 2, {}); });
  chars::BattleRecords bad;
  bad.values[42] = 0x80000000;
  Rejects([&] { rooms::BattleRecordAwards::Apply(r, 0, bad); });
  std::cout << "Battle awards policy: " << checks << " checks\n";
  return checks;
}
