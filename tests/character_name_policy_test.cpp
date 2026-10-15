#include <stdexcept>
#include <string>

#include "features/characters/character_creation.h"

namespace {
using bubble_fighter::server::features::characters::CharacterCreation;
std::u16string Repeat(char16_t value, std::size_t count) {
  return std::u16string(count, value);
}
void Require(bool value) {
  if (!value) throw std::runtime_error("Character name policy test failed.");
}
}  // namespace

int RunCharacterNamePolicyTests() {
  int checks = 0;
  const auto accept = [&checks](std::u16string_view name) {
    ++checks;
    Require(CharacterCreation::ValidateName(name) == 0);
  };
  const auto reject = [&checks](std::u16string_view name, std::uint32_t code) {
    ++checks;
    Require(CharacterCreation::ValidateName(name) == code);
  };
  // Operator 2026-10-15: 2 to 20 display columns, a CJK character counts as two.
  reject(u"", 6);
  reject(u"a", 6);
  accept(u"ab");
  accept(u"泡");
  accept(u"泡泡战士");
  accept(Repeat(u'泡', 10));
  reject(Repeat(u'泡', 11), 6);
  accept(Repeat(u'a', 20));
  reject(Repeat(u'a', 21), 6);
  accept(u"泡泡_1");
  reject(u"泡 泡", 3);
  reject(u"泡泡<战", 3);
  // The character-set check keeps callers with their own length policy working.
  Require(CharacterCreation::ValidateCharacters(u"a") == 0);
  Require(CharacterCreation::ValidateCharacters(u" ") == 3);
  ++checks;
  ++checks;
  return checks + 4;
}
