# ESPHome PS/2 Keyboard component

An external [ESPHome](https://esphome.io/) component that exposes an ESP32 GPIO pair as a PS/2 keyboard. It can type text, send key strokes and combinations, and transmit arbitrary Scan Code Set 2 byte sequences to a host, BIOS, KVM, or vintage computer.

The implementation targets the ESP32-S3 and uses ESP-IDF or Arduino. It is build-tested with ESPHome 2026.9.0; electrical and host compatibility still needs to be verified with the intended hardware.

## Features

- Scan Code Set 2 device-to-host and host-to-device framing with odd parity.
- Non-blocking output from ESPHome automations and Home Assistant actions; a dedicated FreeRTOS task owns the bus.
- Power-on BAT response (`0xAA`) and host reset handling.
- Host commands for enable/disable, defaults, LEDs, echo, resend, device ID, typematic parameters, and scan-code-set queries.
- Caps Lock, Num Lock, and Scroll Lock binary sensors.
- `on_led_change` and `on_host_reset` automation triggers.
- US-layout ASCII typing with automatic Shift handling.
- Key combinations and raw scan-code actions.
- Optional Home Assistant custom API services.

## Supported environment

| Item | Status |
| --- | --- |
| ESP32-S3 | Supported target |
| Other ESP32 variants | Compile-time compatible; verify pin and core support for the board |
| ESP-IDF | Build-tested with ESPHome 2026.9.0 |
| Arduino | Build-tested with ESPHome 2026.9.0 |
| Scan code set | Set 2 |
| Keyboard layout | US ASCII |
| Unicode/layout switching | Not implemented |

This is a keyboard emulator, not a USB keyboard. It cannot enumerate a USB host or replace the host's USB HID stack. Media, power, and extended keys are sent using their Set 2 sequences, but support depends on the receiving BIOS/OS/KVM.

## Hardware and wiring

### Mini-DIN pinout

```text
         ____
       /  __  \
      |  [__]  |
     | 5 6  NC |
     |  3   4  |
     |   1 2   |
      \________/
        Female port
```

| Pin | Signal | ESP32 connection |
| ---: | --- | --- |
| 1 | DATA | Low-voltage side of a bidirectional level shifter |
| 2 | NC | Do not connect |
| 3 | GND | Common ground |
| 4 | +5 V | Host supply, if used by the level shifter |
| 5 | CLK | Low-voltage side of a bidirectional level shifter |
| 6 | NC | Do not connect |

> **Do not connect a 5 V PS/2 signal directly to an ESP32 GPIO.** Use a two-channel bidirectional logic-level shifter (for example, a properly wired BSS138 module), with the low-voltage side powered by 3.3 V and the high-voltage side powered by 5 V. Connect the ground of the shifter, ESP32, and host together. The shifter should have pull-ups on both sides; the component also enables the ESP32-side pull-ups while initializing the pins.

The component starts the lines as pulled-up inputs and switches them to bidirectional open-drain GPIO modes during setup. Keep the shifter and wiring short; PS/2 is an old, slow, single-ended bus.

## Installation

### GitHub external component

```yaml
external_components:
  - source: github://milesoberstadt/esphome-ps2-keyboard
    refresh: 1d
    components: [ps2_keyboard]
```

Pin a release tag or commit in a production configuration if reproducible updates are important.

### Local checkout

```yaml
external_components:
  - source:
      type: local
      path: components
```

Copy the `components/ps2_keyboard` directory into an existing ESPHome external-components directory if that is more convenient.

## Minimal configuration

```yaml
esphome:
  name: ps2-keyboard
  min_version: 2026.9.0

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf  # or arduino

external_components:
  - source: github://milesoberstadt/esphome-ps2-keyboard
    components: [ps2_keyboard]

logger:

ps2_keyboard:
  id: ps2_kb
  clk_pin: GPIO4
  data_pin: GPIO5
```

`task_priority` defaults to `10`; valid values are 1–24. `task_core` defaults to `-1` (no affinity), which is portable across ESP32 variants. Set it to `0` or `1` when a board-specific measurement shows that affinity is useful. If more than one `ps2_keyboard` instance is configured, give each instance a distinct `service_prefix` when custom API services are enabled.

### LED entities and triggers

The LED entities are optional child binary sensors:

```yaml
ps2_keyboard:
  id: ps2_kb
  clk_pin: GPIO4
  data_pin: GPIO5

  caps_lock:
    name: PS2 Caps Lock
  num_lock:
    name: PS2 Num Lock
  scroll_lock:
    name: PS2 Scroll Lock

  on_led_change:
    - lambda: |-
        ESP_LOGI("ps2", "LEDs: caps=%d num=%d scroll=%d", caps, num, scroll);
  on_host_reset:
    - lambda: |-
        ESP_LOGI("ps2", "The host sent a keyboard reset command");
```

`on_host_reset` is triggered by PS/2 command `0xFF`; it is not an electrical detection of every host power cycle.

## ESPHome actions

Actions enqueue a complete output transaction. A transaction is not interleaved with another action submitted at the same time, although the host can still send PS/2 commands while the transaction is in progress.

### Print text

```yaml
on_press:
  - ps2_keyboard.print:
      text: "sudo reboot\n"
      delay: 10ms  # delay between characters
```

`delay` is the inter-character delay. The action returns after queueing the transaction; it does not block the ESPHome loop while the bus is clocked.

### Stroke, press, and release

```yaml
on_press:
  - ps2_keyboard.stroke:
      key: ENTER
      delay: 20ms  # hold time before the break sequence
  - ps2_keyboard.press:
      key: LSHIFT
  - ps2_keyboard.stroke:
      key: A
  - ps2_keyboard.release:
      key: LSHIFT
```

`press` and `release` are separate manual transactions, so another action can be queued between them. Use `stroke` for an atomic make/hold/break operation.

### Combinations

```yaml
on_press:
  - ps2_keyboard.combination:
      keys: [CTRL, ALT, DELETE]
      delay: 50ms
```

A string is also accepted:

```yaml
- ps2_keyboard.combination:
    keys: WIN+R
```

The parser is strict: an unknown token rejects the whole combination. `PAGE_UP` is one key, and the physical minus and plus keys can be used (`CTRL+-` and `CTRL++`).

### Raw scan-code bytes

```yaml
on_press:
  - ps2_keyboard.send_raw:
      bytes: [0x1C, 0xF0, 0x1C]  # A make, then A break
```

The Home Assistant service accepts whitespace-, comma-, or semicolon-separated hexadecimal bytes, for example `E0 75 E0 F0 75`.

## Queue and host-command behavior

- The output queue holds up to 16 complete transactions.
- Text is limited to 1024 bytes and raw input to 4096 bytes per transaction.
- A full queue, disabled reporting, or an allocation failure rejects the new action and logs a warning; the ESPHome/API caller is not blocked waiting for bus space.
- A byte that cannot be sent within 500 ms is retried rather than abandoning a partially transmitted make/break sequence. The task continues servicing host commands and ESPHome remains responsive.
- Host reset, disable, and set-defaults commands discard pending output transactions so stale keystrokes are not sent after a host state change.
- Host commands are serviced during inter-byte and hold delays, rather than waiting for a long action delay to finish.

## Home Assistant services

Custom API services are optional. Enable them explicitly in the API configuration:

```yaml
api:
  custom_services: true
  encryption:
    key: !secret api_encryption_key
```

When `api:` is absent, or `custom_services` is false, the component still compiles and all ESPHome YAML actions work. When enabled, ESPHome exposes these services (the exact node prefix is assigned by ESPHome; `service_prefix` changes the local `ps2_*` portion):

| Service | Parameter | Description |
| --- | --- | --- |
| `esphome.<node>_ps2_type` | `text` | Queue US-ASCII text |
| `esphome.<node>_ps2_stroke` | `key` | Queue a key stroke |
| `esphome.<node>_ps2_press` | `key` | Queue a key-down scan code |
| `esphome.<node>_ps2_release` | `key` | Queue a key-up scan code |
| `esphome.<node>_ps2_combination` | `keys` | Queue a combination such as `CTRL+ALT+DELETE` |
| `esphome.<node>_ps2_send_raw` | `hex_bytes` | Queue raw hexadecimal bytes |

For an unencrypted development setup, omit `encryption`; do not use an unencrypted API on an untrusted network. Never commit an API encryption key or Wi-Fi password.

## Key names

Names are case-insensitive. Common aliases are accepted, including `CTRL`/`LCTRL`, `SHIFT`/`LSHIFT`, `WIN`/`GUI`, `DEL`/`DELETE`, and `PGDN`/`PAGE_DOWN`.

| Category | Names |
| --- | --- |
| Letters | `A`–`Z` |
| Digits | `0`–`9` |
| Modifiers | `CTRL`, `RCTRL`, `SHIFT`, `RSHIFT`, `ALT`, `RALT`/`ALTGR`, `GUI`/`WIN`, `RGUI`, `MENU` |
| Control | `ENTER`, `ESC`, `BACKSPACE`, `TAB`, `SPACE`, `CAPS`, `NUM`, `SCROLL` |
| Navigation | `UP`, `DOWN`, `LEFT`, `RIGHT`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGE_UP`, `PAGE_DOWN` |
| Function | `F1`–`F12` |
| Keypad | `KP0`–`KP9`, `KP_ENTER`, `KP_PLUS`, `KP_MINUS`, `KP_MULTIPLY`, `KP_DIVIDE`, `KP_PERIOD` |
| Media/power | `MUTE`, `VOLUME_UP`, `VOLUME_DOWN`, `PLAY_PAUSE`, `STOP`, `NEXT`, `PREV`, `POWER`, `SLEEP`, `WAKE` |
| Special | `PRINTSCREEN`, `PAUSE` |
| Punctuation | ``BACKQUOTE``, `MINUS`, `EQUALS`, `LEFTBRACKET`, `RIGHTBRACKET`, `BACKSLASH`, `SEMICOLON`, `QUOTE`, `COMMA`, `PERIOD`, `SLASH` |

`print()` supports printable US-ASCII characters, plus tab, newline, carriage return, and backspace. Unsupported bytes are skipped and counted in the log.

## Protocol notes and limitations

The component implements the command/status subset needed by common PC BIOS and operating-system keyboard controllers. It does not implement typematic key repeat, keyboard-controller scan-code sets other than Set 2, dynamic keyboard layouts, or Unicode composition. The host may reject unsupported media/power sequences; use `send_raw` only when the receiver's protocol is known.

All bus timing is performed in the component task. The output queue protects the ESPHome loop from clocking delays, but it cannot guarantee delivery while a host continuously holds the bus or while the host has disabled reporting.

## Development and verification

Run the native parser/scancode tests and ESPHome schema check with:

```sh
./script/test
```

The script requires a C++17 compiler and the `esphome` command. A complete firmware check is:

```sh
esphome config example.yaml
esphome compile example.yaml
```

For release qualification, test both `esp-idf` and `arduino`, and use a logic analyzer or protocol-aware test harness to verify:

1. The power-on `0xAA` response and reset `0xFF` transcript.
2. Odd parity and stop bits on device-to-host frames.
3. Host LED commands and ACK/resend behavior.
4. Make/break sequences for normal, extended, Print Screen, and Pause keys.
5. Cold boot in both a legacy BIOS and a UEFI environment.
6. Bus behavior when the host holds CLK low, sends malformed frames, or disables reporting.

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
