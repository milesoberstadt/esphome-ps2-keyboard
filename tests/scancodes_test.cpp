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
  expect(key_from_string("NOT_A_KEY") == Key::KEY_NONE, "unknown key is rejected");

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
  expect(!parse_key_combination("CTRL+NOT_A_KEY", combination), "invalid combination is rejected");
  expect(combination.empty(), "invalid combination is all-or-nothing");

  expect_bytes(parse_hex_string("E0,75 0x1C;FF"), {0xE0, 0x75, 0x1C, 0xFF}, "valid raw bytes parse");
  expect(parse_hex_string("GG").empty(), "non-hex raw token is rejected");
  expect(parse_hex_string("1G").empty(), "partially valid raw token is rejected");
  expect(parse_hex_string("100").empty(), "raw byte overflow is rejected");
  expect(parse_hex_string("0x").empty(), "empty prefixed raw token is rejected");

  std::cout << "scancodes_test: all checks passed\n";
  return 0;
}
