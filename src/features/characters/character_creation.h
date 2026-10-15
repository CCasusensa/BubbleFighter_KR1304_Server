#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_

#include "features/characters/character_store.h"
#include "protocol/characters/create_user_info_request.h"

namespace bubble_fighter::server::features::characters {
class CharacterCreation {
 public:
  // Explicit local name policy, not reconstructed production rules.
  // Operator 2026-10-15: 2 to 20 display columns, a CJK character counting as
  // two columns — the same rule the launcher's nameWidth applies.
  static constexpr std::size_t kMinNameWidth = 2;
  static constexpr std::size_t kMaxNameWidth = 20;
  // Longest UTF-16 storage an accepted name can need: 20 single-unit columns.
  static constexpr std::size_t kMaxNameCodeUnits = kMaxNameWidth;
  static std::size_t NameWidth(std::u16string_view name);
  // Character set only: 0 when every code unit is an allowed printable scalar.
  // Callers that own their own length policy (clan names, introductions) use
  // this instead of the full width check.
  static std::uint32_t ValidateCharacters(std::u16string_view name);
  static std::uint32_t ValidateName(std::u16string_view name);
  static bool SupportsSelection(std::uint32_t character_type,
                                std::uint32_t color_index);
  explicit CharacterCreation(CharacterStore& store);
  std::uint32_t CheckName(std::u16string_view name);
  std::uint32_t Create(std::uint64_t account_id,
                       const protocol::CreateUserInfoRequest& request);

 private:
  CharacterStore& store_;
};
}  // namespace bubble_fighter::server::features::characters

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_
