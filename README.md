# ESPHome PS/2 Keyboard Component for ESP32-S3

A custom ESPHome external component that emulates a hardware **PS/2 Keyboard** using GPIO pins on an **ESP32-S3** (and other ESP32 family chips). It allows you to send keystrokes, key combinations, arbitrary scan codes, and text strings to any computer, BIOS, server, KVM switch, or retro PC with a PS/2 port, directly from ESPHome automations or Home Assistant.

---

## Features

- **Standard PS/2 Protocol Emulation (Scan Code Set 2)**: Fully implements device-to-host and host-to-device communication.
- **Full Host Command Handling**:
  - Handles `0xFF` (Reset / BAT test) with `0xAA` pass response.
  - Handles `0xF2` (Read Device ID) identifying as a standard keyboard (`0xAB, 0x83`).
  - Handles `0xED` (Set Status Indicators / LEDs) for Caps Lock, Num Lock, and Scroll Lock.
  - Handles `0xEE` (Echo), `0xF4` (Enable), `0xF5` (Disable), `0xF6` (Defaults), `0xFE` (Resend), and `0xF0` (Scan Code Set).
- **Dual Framework Support**: Tested and verified on both `esp-idf` and `arduino` frameworks.
- **Non-blocking FreeRTOS Background Task**: All timing-critical PS/2 clocking and host RTS monitoring runs in a dedicated FreeRTOS background task with a message queue. Your ESPHome loop, WiFi, and Home Assistant API never freeze.
- **Home Assistant Native API Services**: Automatically registers user services in Home Assistant (`ps2_type`, `ps2_stroke`, `ps2_press`, `ps2_release`, `ps2_combination`, `ps2_send_raw`).
- **Binary Sensors & Triggers**:
  - Exposes `caps_lock`, `num_lock`, and `scroll_lock` binary sensors.
  - Automations via `on_led_change` and `on_host_reset` triggers.
- **Rich Keystroke & Hotkey Support**:
  - ASCII text typing with automatic Shift key management.
  - Key combinations (e.g. `Ctrl+Alt+Del`, `Win+R`, `Alt+F4`) with reverse-order release.
  - Raw scan code transmission.

---

## Hardware & Wiring

### PS/2 Mini-DIN 6 Pinout

```
         ____
       /  __  \
      /  [__]  \
     | 5  6  NC |
     |  3   4   |
     |   1 2    |
      \________/
   (Looking at Female Port)
```

| Pin # | PS/2 Signal | Description | Connect To |
|:-----:|:-----------:|:------------|:-----------|
| **1** | **DATA**    | Bidirectional Data Line | Level Shifter HV1 / ESP32 GPIO |
| **2** | *NC*        | Not Connected / Reserved | — |
| **3** | **GND**     | Ground | ESP32 GND & Power Supply GND |
| **4** | **+5V VCC** | 5V Power from Host (Optional) | 5V Level Shifter HV / 5V In |
| **5** | **CLK**     | Bidirectional Clock Line | Level Shifter HV2 / ESP32 GPIO |
| **6** | *NC*        | Not Connected / Reserved | — |

> [!WARNING]
> **Voltage Level Warning**: Most vintage and modern PC PS/2 ports operate at **5V** and have host-side pull-up resistors to +5V. ESP32-S3 GPIO pins operate at **3.3V**.
> Connecting 5V directly to ESP32 pins can degrade or damage the chip.
> **It is strongly recommended to use a 2-channel bidirectional I2C/logic level shifter (such as BSS138-based modules)**:
> - **LV side**: Powered by ESP32 **3.3V**; LV1 connected to `DATA_PIN`, LV2 connected to `CLK_PIN`.
> - **HV side**: Powered by Host **5V**; HV1 connected to PS/2 **DATA**, HV2 connected to PS/2 **CLK**.
> - **GND**: Common ground connected between ESP32 and Host.

---

## Installation & Configuration

1. Place the `components/ps2_keyboard` folder in your ESPHome configuration directory:
   ```text
   your-esphome-config/
   ├── components/
   │   └── ps2_keyboard/
   │       ├── __init__.py
   │       ├── automation.h
   │       ├── binary_sensor.py
   │       ├── ps2_keyboard.cpp
   │       ├── ps2_keyboard.h
   │       └── scancodes.h
   └── my_keyboard.yaml
   ```

2. Include the component in your YAML configuration:

```yaml
esphome:
  name: ps2-keyboard-controller

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf # or 'arduino'

external_components:
  - source:
      type: local
      path: components

api:
  encryption:
    key: "YOUR_ENCRYPTION_KEY_HERE"

wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"

ps2_keyboard:
  id: ps2_kb
  clk_pin: GPIO4
  data_pin: GPIO5

  # Optional settings
  task_priority: 10 # FreeRTOS priority (default: 10)
  task_core: 1      # Core affinity (default: 1, -1 for no affinity)

  # Optional LED status sensors
  caps_lock:
    name: "PS/2 Caps Lock"
  num_lock:
    name: "PS/2 Num Lock"
  scroll_lock:
    name: "PS/2 Scroll Lock"

  # Optional event triggers
  on_led_change:
    - lambda: |-
        ESP_LOGI("ps2", "LEDs: Caps=%d, Num=%d, Scroll=%d", caps, num, scroll);
  on_host_reset:
    - lambda: |-
        ESP_LOGI("ps2", "Host computer booted or reset!");
```

---

## ESPHome Automation Actions

### 1. `ps2_keyboard.print`
Types out an ASCII string character by character, automatically holding Shift for uppercase and punctuation symbols.

```yaml
on_press:
  - ps2_keyboard.print:
      text: "sudo reboot\n"
      delay: 15ms # Optional delay between characters (default: 10ms)
```

### 2. `ps2_keyboard.stroke`
Presses and then releases a single key after a configurable delay.

```yaml
on_press:
  - ps2_keyboard.stroke:
      key: "ENTER"
      delay: 20ms # Optional keypress duration (default: 10ms)
```

### 3. `ps2_keyboard.press` & `ps2_keyboard.release`
Manually hold down and release keys.

```yaml
on_press:
  - ps2_keyboard.press:
      key: "LSHIFT"
  - ps2_keyboard.stroke:
      key: "A"
  - ps2_keyboard.release:
      key: "LSHIFT"
```

### 4. `ps2_keyboard.combination`
Presses multiple modifier and action keys sequentially, pauses, and releases them in reverse order. Can take a list or a string.

```yaml
# List syntax:
on_press:
  - ps2_keyboard.combination:
      keys: ["CTRL", "ALT", "DELETE"]
      delay: 50ms # Hold duration before releasing

# String syntax:
on_press:
  - ps2_keyboard.combination:
      keys: "WIN+R"
```

### 5. `ps2_keyboard.send_raw`
Sends arbitrary hex scan codes directly to the PS/2 bus.

```yaml
on_press:
  - ps2_keyboard.send_raw:
      bytes: [0x1C, 0xF0, 0x1C] # 'A' make and break
```

---

## Home Assistant Integration

When `api:` is configured, the component registers native user services in Home Assistant. You can trigger them from Home Assistant automations, scripts, or **Developer Tools > Actions**:

| Service | Parameters | Description | Example |
|:---|:---|:---|:---|
| `esphome.<node>_ps2_type` | `text` | Types ASCII text string | `text: "echo 'hello world' > test.txt\n"` |
| `esphome.<node>_ps2_stroke` | `key` | Presses & releases a key | `key: "F11"` or `key: "ENTER"` |
| `esphome.<node>_ps2_combination` | `keys` | Presses key combo and releases in reverse order | `keys: "ctrl+alt+del"` or `keys: "win+r"` |
| `esphome.<node>_ps2_press` | `key` | Holds key down | `key: "LCTRL"` |
| `esphome.<node>_ps2_release` | `key` | Releases held key | `key: "LCTRL"` |
| `esphome.<node>_ps2_send_raw` | `hex_bytes` | Sends space/comma-separated hex bytes | `hex_bytes: "E0 75 E0 F0 75"` (Up Arrow) |

---

## Key Name Reference

Key names can be passed case-insensitively to `key:` or `keys:`:

| Category | Available Key Names |
|:---|:---|
| **Letters** | `A` through `Z` |
| **Numbers** | `0` through `9` |
| **Modifiers** | `CTRL` (`LCTRL`), `RCTRL`, `SHIFT` (`LSHIFT`), `RSHIFT`, `ALT` (`LALT`), `RALT` (`ALTGR`), `GUI` (`WIN`, `SUPER`, `CMD`), `RGUI`, `MENU` (`APPS`) |
| **Control** | `ENTER` (`RETURN`), `ESC` (`ESCAPE`), `BACKSPACE` (`BS`), `TAB`, `SPACE` (`SPACEBAR`), `CAPS` (`CAPSLOCK`), `NUM` (`NUMLOCK`), `SCROLL` (`SCROLLLOCK`) |
| **Navigation** | `UP`, `DOWN`, `LEFT`, `RIGHT`, `INSERT` (`INS`), `DELETE` (`DEL`), `HOME`, `END`, `PAGE_UP` (`PGUP`), `PAGE_DOWN` (`PGDN`) |
| **Function** | `F1`, `F2`, `F3`, `F4`, `F5`, `F6`, `F7`, `F8`, `F9`, `F10`, `F11`, `F12` |
| **Keypad** | `KP0` through `KP9`, `KP_ENTER`, `KP_PLUS`, `KP_MINUS`, `KP_MULTIPLY`, `KP_DIVIDE`, `KP_PERIOD` |
| **Media** | `MUTE`, `VOLUME_UP` (`VOL_UP`), `VOLUME_DOWN` (`VOL_DOWN`), `PLAY` (`PLAY_PAUSE`), `STOP`, `NEXT` (`NEXT_TRACK`), `PREV` (`PREV_TRACK`) |
| **Power** | `POWER`, `SLEEP`, `WAKE` |
| **Special** | `PRINTSCREEN` (`PRTSC`), `PAUSE` (`BREAK`) |
| **Punctuation** | `BACKQUOTE` (``` ` ```), `MINUS` (`-`), `EQUALS` (`=`), `LEFTBRACKET` (`[`), `RIGHTBRACKET` (`]`), `BACKSLASH` (`\`), `SEMICOLON` (`;`), `QUOTE` (`'`), `COMMA` (`,`), `PERIOD` (`.`), `SLASH` (`/`) |
