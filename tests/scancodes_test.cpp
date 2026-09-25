#include "ps2_keyboard/scancodes.h"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using esphome::ps2_keyboard::Key;
using namespace esphome::ps2_keyboard;

static void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::abort();
  }
}

static void expect_bytes(const std::vector<uint8_t> &actual, std::initializer_list<uint8_t> expected,
                         const char *message) {
  expect(actual == std::vector<uint8_t>(expected), message);
}

static std::vector<uint8_t> make(Key key) {
  uint8_t data[16]{};
  uint8_t length = 0;
  expect(get_make_code(key, data, length), "make code should exist");
  return std::vector<uint8_t>(data, data + length);
}

static std::vector<uint8_t> release(Key key) {
  uint8_t data[16]{};
  uint8_t length = 0;
  expect(get_break_code(key, data, length), "break code should exist");
  return std::vector<uint8_t>(data, data + length);
}

int main() {
  for (unsigned int value = 0; value <= 0xFF; value++) {
    unsigned int ones = 0;
    for (unsigned int bit = 0; bit < 8; bit++)
      ones += (value >> bit) & 1U;
    expect((ones + ps2_odd_parity(static_cast<uint8_t>(value))) % 2U == 1U, "odd PS/2 parity");
  }

  // Every enum entry except the sentinel and count has a table entry.
  for (uint8_t value = static_cast<uint8_t>(Key::KEY_A); value < static_cast<uint8_t>(Key::_KEY_COUNT); value++) {
    expect(find_key_entry(static_cast<Key>(value)) != nullptr, "every key has a scan-code entry");
  }

  expect_bytes(make(Key::KEY_A), {0x1C}, "A make code");
  expect_bytes(release(Key::KEY_A), {0xF0, 0x1C}, "A break code");
  expect_bytes(make(Key::KEY_DELETE), {0xE0, 0x71}, "extended Delete make code");
  expect_bytes(release(Key::KEY_DELETE), {0xE0, 0xF0, 0x71}, "extended Delete break code");
  expect_bytes(make(Key::KEY_PRINTSCREEN), {0xE0, 0x12, 0xE0, 0x7C}, "Print Screen make code");
  expect_bytes(release(Key::KEY_PRINTSCREEN), {0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12}, "Print Screen break code");
  expect_bytes(make(Key::KEY_PAUSE), {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77}, "Pause make code");
  expect(release(Key::KEY_PAUSE).empty(), "Pause has no break code");

  expect(key_from_string("a") == Key::KEY_A, "case-insensitive letter key");
  expect(key_from_string("LEFTBRACKET") == Key::KEY_LEFTBRACKET, "long punctuation key name");
  expect(key_from_string("PAGE_UP") == Key::KEY_PAGE_UP, "navigation key name");
  expect(key_from_string("+") == Key::KEY_EQUALS, "physical plus key");
  Key parsed_key = Key::KEY_NONE;
  bool parsed_shift = false;
  expect(key_from_string_with_shift("+", parsed_key, parsed_shift) && parsed_key == Key::KEY_EQUALS && parsed_shift,
         "physical plus key carries Shift for key actions");
  expect(key_from_string_with_shift("PLUS", parsed_key, parsed_shift) && parsed_key == Key::KEY_EQUALS && parsed_shift,
         "PLUS alias carries Shift for key actions");
  expect(key_from_string("NOT_A_KEY") == Key::KEY_NONE, "unknown key is rejected");
  expect(key_from_string(std::string(65, 'A')) == Key::KEY_NONE, "overlong key names are rejected");

  expect(is_set3_make_code(0x01) && is_set3_make_code(0x02) && is_set3_make_code(0x7E),
         "Set-3 conventional make codes should be accepted permissively");
  expect(is_set3_make_code(0x83) && is_set3_make_code(0x87), "Set-3 extended 0x83..0x87 codes should be accepted");
  expect(is_set3_make_code(0x8B) && is_set3_make_code(0x8C) && is_set3_make_code(0x8D),
         "Set-3 GUI/application codes should be accepted");
  for (uint8_t invalid : {0x00, 0x7F, 0x80, 0x81, 0x82, 0x88, 0x89, 0x8A, 0x8E, 0xFF}) {
    expect(!is_set3_make_code(invalid), "reserved or non-Set-3 make code should be rejected");
  }

  for (unsigned char c = 0x20; c <= 0x7E; c++) {
    Key key = Key::KEY_NONE;
    bool shift = false;
    expect(ascii_to_key(static_cast<char>(c), key, shift), "printable US-ASCII character is mapped");
    expect(key != Key::KEY_NONE, "printable character has a physical key");
  }
  Key unsupported_key = Key::KEY_NONE;
  bool unsupported_shift = false;
  expect(!ascii_to_key('\x01', unsupported_key, unsupported_shift), "control bytes are not printable characters");

  std::vector<Key> combination;
  expect(parse_key_combination("CTRL+ALT+DELETE", combination), "valid combination parses");
  expect(combination.size() == 3, "valid combination has three keys");
  expect(combination[0] == Key::KEY_LCTRL && combination[1] == Key::KEY_LALT && combination[2] == Key::KEY_DELETE,
         "combination order is preserved");
  expect(parse_key_combination("PAGE_UP", combination) && combination.size() == 1 && combination[0] == Key::KEY_PAGE_UP,
         "underscores in key names are preserved");
  expect(parse_key_combination("CTRL+-", combination) && combination.size() == 2 && combination[1] == Key::KEY_MINUS,
         "minus can be a combination key");
  expect(parse_key_combination("CTRL++", combination) && combination.size() == 2 && combination[1] == Key::KEY_EQUALS,
         "terminal plus represents the plus key");
  std::vector<KeyCombinationPart> shifted_combination;
  expect(parse_key_combination_with_shift("CTRL++", shifted_combination) && shifted_combination.size() == 2 &&
             shifted_combination[1].key == Key::KEY_EQUALS && shifted_combination[1].shift,
         "terminal plus retains Shift in parsed combination actions");
  expect(!parse_key_combination("CTRL+NOT_A_KEY", combination), "invalid combination is rejected");
  expect(combination.empty(), "invalid combination is all-or-nothing");
  expect(!parse_key_combination("CTRL+", combination), "a single trailing separator is rejected");
  expect(combination.empty(), "invalid combination is all-or-nothing");
  std::string overlong_combination;
  for (int i = 0; i < 33; i++) {
    if (i != 0)
      overlong_combination += '+';
    overlong_combination += 'A';
  }
  expect(!parse_key_combination(overlong_combination, combination), "combinations are limited to 32 keys");
  expect(combination.empty(), "overlong combination is all-or-nothing");

  expect_bytes(parse_hex_string("E0,75 0x1C;0xff;FF"), {0xE0, 0x75, 0x1C, 0xFF, 0xFF},
               "valid raw bytes parse with delimiters and prefix case");
  expect(parse_hex_string("GG").empty(), "non-hex raw token is rejected");
  expect(parse_hex_string("1G").empty(), "partially valid raw token is rejected");
  expect(parse_hex_string("100").empty(), "raw byte overflow is rejected");
  expect(parse_hex_string("0x").empty(), "empty prefixed raw token is rejected");
  expect_bytes(parse_hex_string(std::string(32, '0')), {0x00}, "32-character zero-padded raw token is accepted");
  expect(parse_hex_string(std::string(32, '1')).empty(), "large raw token must not overflow silently");
  expect(parse_hex_string(std::string(33, '0')).empty(), "overlong raw token is rejected");
  expect(parse_hex_string("00 01 02", 2).empty(), "raw parser enforces its output limit");
  expect_bytes(parse_hex_string("FF, FF;", 2), {0xFF, 0xFF}, "raw parser ignores empty delimiters");

  const size_t max_raw_bytes = 4096;
  std::string max_raw_input;
  max_raw_input.reserve(max_raw_bytes * 3);
  for (size_t i = 0; i < max_raw_bytes; i++)
    max_raw_input += "FF ";
  expect(max_raw_input.size() <= MAX_HEX_INPUT_CHARS, "maximum raw HA input should fit the input limit");
  expect(parse_hex_string(max_raw_input, max_raw_bytes, max_raw_input.size()).size() == max_raw_bytes,
         "raw parser accepts input at its character limit");
  expect(parse_hex_string(max_raw_input, max_raw_bytes, max_raw_input.size() - 1).empty(),
         "raw parser rejects input over its character limit");
  expect(parse_hex_string(" \t\r\n,;", max_raw_bytes, MAX_HEX_INPUT_CHARS).empty(),
         "empty raw input is rejected by the action path");

  std::cout << "scancodes_test: all checks passed\n";
  return 0;
}
