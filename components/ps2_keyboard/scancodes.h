#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace esphome {
namespace ps2_keyboard {

enum class Key : uint8_t {
  KEY_NONE = 0,

  // Letters
  KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
  KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
  KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

  // Digits
  KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,

  // Punctuation & symbols
  KEY_BACKQUOTE, KEY_MINUS, KEY_EQUALS, KEY_BACKSLASH, KEY_BACKSPACE,
  KEY_SPACE, KEY_TAB, KEY_CAPSLOCK,
  KEY_LEFTBRACKET, KEY_RIGHTBRACKET, KEY_SEMICOLON, KEY_QUOTE,
  KEY_COMMA, KEY_PERIOD, KEY_SLASH,

  // Modifiers
  KEY_LSHIFT, KEY_LCTRL, KEY_LGUI, KEY_LALT,
  KEY_RSHIFT, KEY_RCTRL, KEY_RGUI, KEY_RALT,
  KEY_MENU,

  // Control & Navigation
  KEY_ENTER, KEY_ESC,
  KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
  KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
  KEY_PRINTSCREEN, KEY_SCROLLLOCK, KEY_PAUSE,

  // Navigation cluster
  KEY_INSERT, KEY_HOME, KEY_PAGE_UP, KEY_DELETE, KEY_END, KEY_PAGE_DOWN,
  KEY_UP, KEY_LEFT, KEY_DOWN, KEY_RIGHT,

  // Keypad
  KEY_NUMLOCK, KEY_KP_DIVIDE, KEY_KP_MULTIPLY, KEY_KP_MINUS,
  KEY_KP_PLUS, KEY_KP_ENTER, KEY_KP_PERIOD,
  KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
  KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,

  // Power / Media
  KEY_POWER, KEY_SLEEP, KEY_WAKE,
  KEY_NEXT_TRACK, KEY_PREV_TRACK, KEY_STOP, KEY_PLAY_PAUSE,
  KEY_MUTE, KEY_VOLUME_UP, KEY_VOLUME_DOWN,

  _KEY_COUNT
};

enum class KeyType : uint8_t {
  NORMAL,               // Make: [code], Break: [0xF0, code]
  EXTENDED,             // Make: [0xE0, code], Break: [0xE0, 0xF0, code]
  SPECIAL_PRINTSCREEN,  // Make: [0xE0, 0x12, 0xE0, 0x7C], Break: [0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12]
  SPECIAL_PAUSE         // Make: [0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77], Break: none
};

struct KeyEntry {
  Key key;
  KeyType type;
  uint8_t code;
};

// Scan Code Set 2 Table
static const KeyEntry KEY_MAP[] = {
    // Letters
    {Key::KEY_A, KeyType::NORMAL, 0x1C},
    {Key::KEY_B, KeyType::NORMAL, 0x32},
    {Key::KEY_C, KeyType::NORMAL, 0x21},
    {Key::KEY_D, KeyType::NORMAL, 0x23},
    {Key::KEY_E, KeyType::NORMAL, 0x24},
    {Key::KEY_F, KeyType::NORMAL, 0x2B},
    {Key::KEY_G, KeyType::NORMAL, 0x34},
    {Key::KEY_H, KeyType::NORMAL, 0x33},
    {Key::KEY_I, KeyType::NORMAL, 0x43},
    {Key::KEY_J, KeyType::NORMAL, 0x3B},
    {Key::KEY_K, KeyType::NORMAL, 0x42},
    {Key::KEY_L, KeyType::NORMAL, 0x4B},
    {Key::KEY_M, KeyType::NORMAL, 0x3A},
    {Key::KEY_N, KeyType::NORMAL, 0x31},
    {Key::KEY_O, KeyType::NORMAL, 0x44},
    {Key::KEY_P, KeyType::NORMAL, 0x4D},
    {Key::KEY_Q, KeyType::NORMAL, 0x15},
    {Key::KEY_R, KeyType::NORMAL, 0x2D},
    {Key::KEY_S, KeyType::NORMAL, 0x1B},
    {Key::KEY_T, KeyType::NORMAL, 0x2C},
    {Key::KEY_U, KeyType::NORMAL, 0x3C},
    {Key::KEY_V, KeyType::NORMAL, 0x2A},
    {Key::KEY_W, KeyType::NORMAL, 0x1D},
    {Key::KEY_X, KeyType::NORMAL, 0x22},
    {Key::KEY_Y, KeyType::NORMAL, 0x35},
    {Key::KEY_Z, KeyType::NORMAL, 0x1A},

    // Digits
    {Key::KEY_0, KeyType::NORMAL, 0x45},
    {Key::KEY_1, KeyType::NORMAL, 0x16},
    {Key::KEY_2, KeyType::NORMAL, 0x1E},
    {Key::KEY_3, KeyType::NORMAL, 0x26},
    {Key::KEY_4, KeyType::NORMAL, 0x25},
    {Key::KEY_5, KeyType::NORMAL, 0x2E},
    {Key::KEY_6, KeyType::NORMAL, 0x36},
    {Key::KEY_7, KeyType::NORMAL, 0x3D},
    {Key::KEY_8, KeyType::NORMAL, 0x3E},
    {Key::KEY_9, KeyType::NORMAL, 0x46},

    // Punctuation & symbols
    {Key::KEY_BACKQUOTE, KeyType::NORMAL, 0x0E},
    {Key::KEY_MINUS, KeyType::NORMAL, 0x4E},
    {Key::KEY_EQUALS, KeyType::NORMAL, 0x55},
    {Key::KEY_BACKSLASH, KeyType::NORMAL, 0x5D},
    {Key::KEY_BACKSPACE, KeyType::NORMAL, 0x66},
    {Key::KEY_SPACE, KeyType::NORMAL, 0x29},
    {Key::KEY_TAB, KeyType::NORMAL, 0x0D},
    {Key::KEY_CAPSLOCK, KeyType::NORMAL, 0x58},
    {Key::KEY_LEFTBRACKET, KeyType::NORMAL, 0x54},
    {Key::KEY_RIGHTBRACKET, KeyType::NORMAL, 0x5B},
    {Key::KEY_SEMICOLON, KeyType::NORMAL, 0x4C},
    {Key::KEY_QUOTE, KeyType::NORMAL, 0x52},
    {Key::KEY_COMMA, KeyType::NORMAL, 0x41},
    {Key::KEY_PERIOD, KeyType::NORMAL, 0x49},
    {Key::KEY_SLASH, KeyType::NORMAL, 0x4A},

    // Modifiers
    {Key::KEY_LSHIFT, KeyType::NORMAL, 0x12},
    {Key::KEY_LCTRL, KeyType::NORMAL, 0x14},
    {Key::KEY_LGUI, KeyType::EXTENDED, 0x1F},
    {Key::KEY_LALT, KeyType::NORMAL, 0x11},
    {Key::KEY_RSHIFT, KeyType::NORMAL, 0x59},
    {Key::KEY_RCTRL, KeyType::EXTENDED, 0x14},
    {Key::KEY_RGUI, KeyType::EXTENDED, 0x27},
    {Key::KEY_RALT, KeyType::EXTENDED, 0x11},
    {Key::KEY_MENU, KeyType::EXTENDED, 0x2F},

    // Control & Navigation
    {Key::KEY_ENTER, KeyType::NORMAL, 0x5A},
    {Key::KEY_ESC, KeyType::NORMAL, 0x76},
    {Key::KEY_F1, KeyType::NORMAL, 0x05},
    {Key::KEY_F2, KeyType::NORMAL, 0x06},
    {Key::KEY_F3, KeyType::NORMAL, 0x04},
    {Key::KEY_F4, KeyType::NORMAL, 0x0C},
    {Key::KEY_F5, KeyType::NORMAL, 0x03},
    {Key::KEY_F6, KeyType::NORMAL, 0x0B},
    {Key::KEY_F7, KeyType::NORMAL, 0x83},
    {Key::KEY_F8, KeyType::NORMAL, 0x0A},
    {Key::KEY_F9, KeyType::NORMAL, 0x01},
    {Key::KEY_F10, KeyType::NORMAL, 0x09},
    {Key::KEY_F11, KeyType::NORMAL, 0x78},
    {Key::KEY_F12, KeyType::NORMAL, 0x07},
    {Key::KEY_PRINTSCREEN, KeyType::SPECIAL_PRINTSCREEN, 0},
    {Key::KEY_SCROLLLOCK, KeyType::NORMAL, 0x7E},
    {Key::KEY_PAUSE, KeyType::SPECIAL_PAUSE, 0},

    // Navigation cluster
    {Key::KEY_INSERT, KeyType::EXTENDED, 0x70},
    {Key::KEY_HOME, KeyType::EXTENDED, 0x6C},
    {Key::KEY_PAGE_UP, KeyType::EXTENDED, 0x7D},
    {Key::KEY_DELETE, KeyType::EXTENDED, 0x71},
    {Key::KEY_END, KeyType::EXTENDED, 0x69},
    {Key::KEY_PAGE_DOWN, KeyType::EXTENDED, 0x7A},
    {Key::KEY_UP, KeyType::EXTENDED, 0x75},
    {Key::KEY_LEFT, KeyType::EXTENDED, 0x6B},
    {Key::KEY_DOWN, KeyType::EXTENDED, 0x72},
    {Key::KEY_RIGHT, KeyType::EXTENDED, 0x74},

    // Keypad
    {Key::KEY_NUMLOCK, KeyType::NORMAL, 0x77},
    {Key::KEY_KP_DIVIDE, KeyType::EXTENDED, 0x4A},
    {Key::KEY_KP_MULTIPLY, KeyType::NORMAL, 0x7C},
    {Key::KEY_KP_MINUS, KeyType::NORMAL, 0x7B},
    {Key::KEY_KP_PLUS, KeyType::NORMAL, 0x79},
    {Key::KEY_KP_ENTER, KeyType::EXTENDED, 0x5A},
    {Key::KEY_KP_PERIOD, KeyType::NORMAL, 0x71},
    {Key::KEY_KP0, KeyType::NORMAL, 0x70},
    {Key::KEY_KP1, KeyType::NORMAL, 0x69},
    {Key::KEY_KP2, KeyType::NORMAL, 0x72},
    {Key::KEY_KP3, KeyType::NORMAL, 0x7A},
    {Key::KEY_KP4, KeyType::NORMAL, 0x6B},
    {Key::KEY_KP5, KeyType::NORMAL, 0x73},
    {Key::KEY_KP6, KeyType::NORMAL, 0x74},
    {Key::KEY_KP7, KeyType::NORMAL, 0x6C},
    {Key::KEY_KP8, KeyType::NORMAL, 0x75},
    {Key::KEY_KP9, KeyType::NORMAL, 0x7D},

    // Power / Media
    {Key::KEY_POWER, KeyType::EXTENDED, 0x37},
    {Key::KEY_SLEEP, KeyType::EXTENDED, 0x3F},
    {Key::KEY_WAKE, KeyType::EXTENDED, 0x5E},
    {Key::KEY_NEXT_TRACK, KeyType::EXTENDED, 0x4D},
    {Key::KEY_PREV_TRACK, KeyType::EXTENDED, 0x15},
    {Key::KEY_STOP, KeyType::EXTENDED, 0x3B},
    {Key::KEY_PLAY_PAUSE, KeyType::EXTENDED, 0x34},
    {Key::KEY_MUTE, KeyType::EXTENDED, 0x23},
    {Key::KEY_VOLUME_UP, KeyType::EXTENDED, 0x32},
    {Key::KEY_VOLUME_DOWN, KeyType::EXTENDED, 0x21},
};

static inline const KeyEntry *find_key_entry(Key key) {
  for (const auto &entry : KEY_MAP) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

inline bool get_make_code(Key key, uint8_t *out_data, uint8_t &out_len) {
  const KeyEntry *entry = find_key_entry(key);
  if (!entry) {
    out_len = 0;
    return false;
  }
  switch (entry->type) {
    case KeyType::NORMAL:
      out_data[0] = entry->code;
      out_len = 1;
      return true;
    case KeyType::EXTENDED:
      out_data[0] = 0xE0;
      out_data[1] = entry->code;
      out_len = 2;
      return true;
    case KeyType::SPECIAL_PRINTSCREEN:
      out_data[0] = 0xE0;
      out_data[1] = 0x12;
      out_data[2] = 0xE0;
      out_data[3] = 0x7C;
      out_len = 4;
      return true;
    case KeyType::SPECIAL_PAUSE:
      out_data[0] = 0xE1;
      out_data[1] = 0x14;
      out_data[2] = 0x77;
      out_data[3] = 0xE1;
      out_data[4] = 0xF0;
      out_data[5] = 0x14;
      out_data[6] = 0xF0;
      out_data[7] = 0x77;
      out_len = 8;
      return true;
    default:
      out_len = 0;
      return false;
  }
}

inline bool get_break_code(Key key, uint8_t *out_data, uint8_t &out_len) {
  const KeyEntry *entry = find_key_entry(key);
  if (!entry) {
    out_len = 0;
    return false;
  }
  switch (entry->type) {
    case KeyType::NORMAL:
      out_data[0] = 0xF0;
      out_data[1] = entry->code;
      out_len = 2;
      return true;
    case KeyType::EXTENDED:
      out_data[0] = 0xE0;
      out_data[1] = 0xF0;
      out_data[2] = entry->code;
      out_len = 3;
      return true;
    case KeyType::SPECIAL_PRINTSCREEN:
      out_data[0] = 0xE0;
      out_data[1] = 0xF0;
      out_data[2] = 0x7C;
      out_data[3] = 0xE0;
      out_data[4] = 0xF0;
      out_data[5] = 0x12;
      out_len = 6;
      return true;
    case KeyType::SPECIAL_PAUSE:
      // Pause has no break code in PS/2 protocol
      out_len = 0;
      return true;
    default:
      out_len = 0;
      return false;
  }
}

inline std::string trim_and_upper(const std::string &str) {
  std::string s = str;
  // Trim leading whitespace
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  // Trim trailing whitespace
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
  // Upper-case
  for (auto &c : s) {
    c = std::toupper((unsigned char)c);
  }
  return s;
}

inline Key key_from_string(const std::string &raw_str) {
  std::string s = trim_and_upper(raw_str);
  if (s.empty()) return Key::KEY_NONE;

  // Single characters
  if (s.length() == 1) {
    char c = s[0];
    if (c >= 'A' && c <= 'Z') return (Key)((uint8_t)Key::KEY_A + (c - 'A'));
    if (c >= '0' && c <= '9') return (Key)((uint8_t)Key::KEY_0 + (c - '0'));
    if (c == ' ') return Key::KEY_SPACE;
    if (c == '`') return Key::KEY_BACKQUOTE;
    if (c == '-') return Key::KEY_MINUS;
    if (c == '=') return Key::KEY_EQUALS;
    if (c == '[') return Key::KEY_LEFTBRACKET;
    if (c == ']') return Key::KEY_RIGHTBRACKET;
    if (c == '\\') return Key::KEY_BACKSLASH;
    if (c == ';') return Key::KEY_SEMICOLON;
    if (c == '\'') return Key::KEY_QUOTE;
    if (c == ',') return Key::KEY_COMMA;
    if (c == '.') return Key::KEY_PERIOD;
    if (c == '/') return Key::KEY_SLASH;
  }

  // Common aliases & function keys
  if (s == "ENTER" || s == "RETURN") return Key::KEY_ENTER;
  if (s == "ESC" || s == "ESCAPE") return Key::KEY_ESC;
  if (s == "BACKSPACE" || s == "BS") return Key::KEY_BACKSPACE;
  if (s == "TAB") return Key::KEY_TAB;
  if (s == "SPACE" || s == "SPACEBAR") return Key::KEY_SPACE;
  if (s == "CAPS" || s == "CAPSLOCK" || s == "CAPS_LOCK") return Key::KEY_CAPSLOCK;
  if (s == "NUM" || s == "NUMLOCK" || s == "NUM_LOCK") return Key::KEY_NUMLOCK;
  if (s == "SCROLL" || s == "SCROLLLOCK" || s == "SCROLL_LOCK") return Key::KEY_SCROLLLOCK;

  // Modifiers
  if (s == "CTRL" || s == "LCTRL" || s == "LEFT_CTRL" || s == "CONTROL") return Key::KEY_LCTRL;
  if (s == "RCTRL" || s == "RIGHT_CTRL") return Key::KEY_RCTRL;
  if (s == "SHIFT" || s == "LSHIFT" || s == "LEFT_SHIFT") return Key::KEY_LSHIFT;
  if (s == "RSHIFT" || s == "RIGHT_SHIFT") return Key::KEY_RSHIFT;
  if (s == "ALT" || s == "LALT" || s == "LEFT_ALT") return Key::KEY_LALT;
  if (s == "RALT" || s == "RIGHT_ALT" || s == "ALTGR") return Key::KEY_RALT;
  if (s == "GUI" || s == "LGUI" || s == "WIN" || s == "WINDOWS" || s == "SUPER" || s == "CMD") return Key::KEY_LGUI;
  if (s == "RGUI" || s == "RWIN") return Key::KEY_RGUI;
  if (s == "MENU" || s == "APPS") return Key::KEY_MENU;

  // Navigation
  if (s == "UP" || s == "ARROW_UP" || s == "UP_ARROW") return Key::KEY_UP;
  if (s == "DOWN" || s == "ARROW_DOWN" || s == "DOWN_ARROW") return Key::KEY_DOWN;
  if (s == "LEFT" || s == "ARROW_LEFT" || s == "LEFT_ARROW") return Key::KEY_LEFT;
  if (s == "RIGHT" || s == "ARROW_RIGHT" || s == "RIGHT_ARROW") return Key::KEY_RIGHT;
  if (s == "INSERT" || s == "INS") return Key::KEY_INSERT;
  if (s == "DELETE" || s == "DEL") return Key::KEY_DELETE;
  if (s == "HOME") return Key::KEY_HOME;
  if (s == "END") return Key::KEY_END;
  if (s == "PAGE_UP" || s == "PAGEUP" || s == "PGUP") return Key::KEY_PAGE_UP;
  if (s == "PAGE_DOWN" || s == "PAGEDOWN" || s == "PGDN") return Key::KEY_PAGE_DOWN;

  // Function keys F1 - F12
  if (s == "F1") return Key::KEY_F1;
  if (s == "F2") return Key::KEY_F2;
  if (s == "F3") return Key::KEY_F3;
  if (s == "F4") return Key::KEY_F4;
  if (s == "F5") return Key::KEY_F5;
  if (s == "F6") return Key::KEY_F6;
  if (s == "F7") return Key::KEY_F7;
  if (s == "F8") return Key::KEY_F8;
  if (s == "F9") return Key::KEY_F9;
  if (s == "F10") return Key::KEY_F10;
  if (s == "F11") return Key::KEY_F11;
  if (s == "F12") return Key::KEY_F12;

  // Print screen & Pause
  if (s == "PRINTSCREEN" || s == "PRTSC" || s == "PRTSCR" || s == "PRINT_SCREEN") return Key::KEY_PRINTSCREEN;
  if (s == "PAUSE" || s == "BREAK" || s == "PAUSE_BREAK") return Key::KEY_PAUSE;

  // Keypad
  if (s == "KP0" || s == "KP_0") return Key::KEY_KP0;
  if (s == "KP1" || s == "KP_1") return Key::KEY_KP1;
  if (s == "KP2" || s == "KP_2") return Key::KEY_KP2;
  if (s == "KP3" || s == "KP_3") return Key::KEY_KP3;
  if (s == "KP4" || s == "KP_4") return Key::KEY_KP4;
  if (s == "KP5" || s == "KP_5") return Key::KEY_KP5;
  if (s == "KP6" || s == "KP_6") return Key::KEY_KP6;
  if (s == "KP7" || s == "KP_7") return Key::KEY_KP7;
  if (s == "KP8" || s == "KP_8") return Key::KEY_KP8;
  if (s == "KP9" || s == "KP_9") return Key::KEY_KP9;
  if (s == "KP_ENTER") return Key::KEY_KP_ENTER;
  if (s == "KP_PLUS" || s == "KP_+") return Key::KEY_KP_PLUS;
  if (s == "KP_MINUS" || s == "KP_-") return Key::KEY_KP_MINUS;
  if (s == "KP_MULTIPLY" || s == "KP_*") return Key::KEY_KP_MULTIPLY;
  if (s == "KP_DIVIDE" || s == "KP_/") return Key::KEY_KP_DIVIDE;
  if (s == "KP_PERIOD" || s == "KP_.") return Key::KEY_KP_PERIOD;

  // Media & Power
  if (s == "MUTE") return Key::KEY_MUTE;
  if (s == "VOL_UP" || s == "VOLUME_UP") return Key::KEY_VOLUME_UP;
  if (s == "VOL_DOWN" || s == "VOLUME_DOWN") return Key::KEY_VOLUME_DOWN;
  if (s == "PLAY" || s == "PLAY_PAUSE") return Key::KEY_PLAY_PAUSE;
  if (s == "STOP") return Key::KEY_STOP;
  if (s == "NEXT" || s == "NEXT_TRACK") return Key::KEY_NEXT_TRACK;
  if (s == "PREV" || s == "PREV_TRACK") return Key::KEY_PREV_TRACK;
  if (s == "POWER") return Key::KEY_POWER;
  if (s == "SLEEP") return Key::KEY_SLEEP;
  if (s == "WAKE") return Key::KEY_WAKE;

  return Key::KEY_NONE;
}

inline bool ascii_to_key(char c, Key &key, bool &shift) {
  shift = false;
  if (c >= 'a' && c <= 'z') {
    key = (Key)((uint8_t)Key::KEY_A + (c - 'a'));
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    key = (Key)((uint8_t)Key::KEY_A + (c - 'A'));
    shift = true;
    return true;
  }
  if (c >= '1' && c <= '9') {
    key = (Key)((uint8_t)Key::KEY_1 + (c - '1'));
    return true;
  }
  if (c == '0') {
    key = Key::KEY_0;
    return true;
  }

  switch (c) {
    case ' ':  key = Key::KEY_SPACE; return true;
    case '\t': key = Key::KEY_TAB; return true;
    case '\n':
    case '\r': key = Key::KEY_ENTER; return true;
    case '\b': key = Key::KEY_BACKSPACE; return true;
    case '`':  key = Key::KEY_BACKQUOTE; return true;
    case '~':  key = Key::KEY_BACKQUOTE; shift = true; return true;
    case '-':  key = Key::KEY_MINUS; return true;
    case '_':  key = Key::KEY_MINUS; shift = true; return true;
    case '=':  key = Key::KEY_EQUALS; return true;
    case '+':  key = Key::KEY_EQUALS; shift = true; return true;
    case '[':  key = Key::KEY_LEFTBRACKET; return true;
    case '{':  key = Key::KEY_LEFTBRACKET; shift = true; return true;
    case ']':  key = Key::KEY_RIGHTBRACKET; return true;
    case '}':  key = Key::KEY_RIGHTBRACKET; shift = true; return true;
    case '\\': key = Key::KEY_BACKSLASH; return true;
    case '|':  key = Key::KEY_BACKSLASH; shift = true; return true;
    case ';':  key = Key::KEY_SEMICOLON; return true;
    case ':':  key = Key::KEY_SEMICOLON; shift = true; return true;
    case '\'': key = Key::KEY_QUOTE; return true;
    case '"':  key = Key::KEY_QUOTE; shift = true; return true;
    case ',':  key = Key::KEY_COMMA; return true;
    case '<':  key = Key::KEY_COMMA; shift = true; return true;
    case '.':  key = Key::KEY_PERIOD; return true;
    case '>':  key = Key::KEY_PERIOD; shift = true; return true;
    case '/':  key = Key::KEY_SLASH; return true;
    case '?':  key = Key::KEY_SLASH; shift = true; return true;
    case '!':  key = Key::KEY_1; shift = true; return true;
    case '@':  key = Key::KEY_2; shift = true; return true;
    case '#':  key = Key::KEY_3; shift = true; return true;
    case '$':  key = Key::KEY_4; shift = true; return true;
    case '%':  key = Key::KEY_5; shift = true; return true;
    case '^':  key = Key::KEY_6; shift = true; return true;
    case '&':  key = Key::KEY_7; shift = true; return true;
    case '*':  key = Key::KEY_8; shift = true; return true;
    case '(':  key = Key::KEY_9; shift = true; return true;
    case ')':  key = Key::KEY_0; shift = true; return true;
    default:
      key = Key::KEY_NONE;
      return false;
  }
}

inline std::vector<Key> parse_key_combination(const std::string &str) {
  std::vector<Key> result;
  std::string token;
  for (size_t i = 0; i <= str.size(); i++) {
    char c = (i < str.size()) ? str[i] : '+';
    if (c == '+' || c == '-') {
      if (!token.empty()) {
        Key k = key_from_string(token);
        if (k != Key::KEY_NONE) {
          result.push_back(k);
        }
        token.clear();
      }
    } else {
      token += c;
    }
  }
  return result;
}

inline std::vector<uint8_t> parse_hex_string(const std::string &hex_str) {
  std::vector<uint8_t> result;
  std::string token;
  auto flush_token = [&]() {
    if (!token.empty()) {
      char *end = nullptr;
      unsigned long val = std::strtoul(token.c_str(), &end, 16);
      if (end != token.c_str()) {
        result.push_back(static_cast<uint8_t>(val & 0xFF));
      }
      token.clear();
    }
  };

  for (char c : hex_str) {
    if (std::isspace((unsigned char)c) || c == ',' || c == ';') {
      flush_token();
    } else {
      token += c;
    }
  }
  flush_token();
  return result;
}

}  // namespace ps2_keyboard
}  // namespace esphome
