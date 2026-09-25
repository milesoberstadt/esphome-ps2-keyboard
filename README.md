# ESPHome PS/2 Keyboard component

An external [ESPHome](https://esphome.io/) component that exposes an ESP32 GPIO pair as a PS/2 keyboard. Type text, send key strokes and combinations, and transmit arbitrary Scan Code Set 2 byte sequences to a host, BIOS, KVM, or vintage computer.

Works on the ESP32-S3 with ESP-IDF or Arduino, and is build-tested with ESPHome 2026.9.0.

Call it AI slop if you like, I just needed to emulate a PS/2 keyboard to turn on a computer that didn't support Wake On LAN.

## Quick start

1. **Wire an ESP32-S3 to a PS/2 port** through a bidirectional level shifter — [see wiring below](#hardware-and-wiring).
2. **Set up your secrets**: copy [`secrets.yaml.example`](secrets.yaml.example) to `secrets.yaml` in the same directory as your config and fill in the values.
3. **Create a config** based on the example below, setting the pins you used.
4. **Flash it** (`esphome run your-config.yaml`), let it join Wi-Fi, then trigger an action to type.

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

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

captive_portal:

logger:

ps2_keyboard:
  id: ps2_kb
  clk_pin: GPIO4
  data_pin: GPIO5

# Something to trigger typing with:
button:
  - platform: template
    name: Type Hello World
    on_press:
      - ps2_keyboard.print:
          text: "Hello from ESP32!\n"
```

Then trigger `ps2_keyboard.print` from Home Assistant, an automation, or the button above. A full working example with more buttons is in [`example.yaml`](example.yaml).

> **What this is and is not.** This is a PS/2 keyboard *emulator*, not a USB keyboard. It cannot enumerate a USB host or replace its HID stack. Media, power, and extended keys are sent as Set 2 sequences, but whether they work depends on the receiving BIOS/OS/KVM. See [limitations](#limitations) at the end.

## Hardware and wiring

### What you need

- An ESP32-S3 (other ESP32 variants compile, but verify pin/core support for your board)
- A two-channel **bidirectional** logic-level shifter (e.g., a BSS138 module wired correctly), one channel per line: CLK and DATA
- A PS/2 Mini-DIN connector on the host side

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

### ⚠️ Do not connect 5 V PS/2 signals directly to an ESP32 GPIO

PS/2 is a 5 V bus; ESP32 GPIOs are 3.3 V tolerant at best. Use a bidirectional shifter with the low-voltage side on 3.3 V and the high-voltage side on 5 V, and tie the shifter, ESP32, and host grounds together. The shifter should have pull-ups on both sides; the component also enables the ESP32-side pull-ups during pin setup.

The component starts the lines as pulled-up inputs and switches them to bidirectional open-drain GPIO modes during setup. Keep the shifter and wiring short — PS/2 is an old, slow, single-ended bus.

## Configuration

### Component options

```yaml
ps2_keyboard:
  id: ps2_kb                 # required: YAML id used by actions
  clk_pin: GPIO4             # required
  data_pin: GPIO5            # required
  service_prefix: ps2        # optional, must be unique across instances
  task_priority: 10          # optional, default 10, valid range 1–24
  task_core: -1              # optional, default -1 (no affinity); set 0 or 1 on ESP32-S3 if measurement shows it helps
```

Two constraints are validated at compile time:

- Every instance must have a distinct `service_prefix`.
- No two instances may reuse the same CLK or DATA GPIO.

### LED sensors and triggers

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

Note: `on_host_reset` is triggered by PS/2 command `0xFF` (a host reset command), not an electrical detection of every power cycle.

## Typing: ESPHome actions

Actions enqueue a complete output transaction. A transaction is not interleaved with another action submitted at the same time, though the host can still send PS/2 commands while it is in progress. The action returns after queueing; it never blocks the ESPHome loop.

### Print text

```yaml
- ps2_keyboard.print:
    text: "sudo reboot\n"
    delay: 10ms  # delay between characters
```

Supports printable US-ASCII plus tab, newline, carriage return, and backspace. Unsupported bytes are skipped and counted in the log. Caps Lock state is sampled while the transaction is built, so changing the host LED state later does not rewrite already-queued text.

### Stroke, press, and release

```yaml
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

Use `stroke` for an atomic make/hold/break. `press` and `release` are separate manual transactions, so another action can be queued between them.

### Combinations

```yaml
- ps2_keyboard.combination:
    keys: [CTRL, ALT, DELETE]
    delay: 50ms
```

A string is also accepted:

```yaml
- ps2_keyboard.combination:
    keys: WIN+R
```

The parser is strict: an unknown token rejects the whole combination, and a combination may contain at most 32 keys. `PAGE_UP` is one key, and the physical minus/plus keys work (`CTRL+-`, `CTRL++`).

### Raw scan-code bytes

```yaml
- ps2_keyboard.send_raw:
    bytes: [0x1C, 0xF0, 0x1C]  # A make, then A break
```

## Home Assistant services

Custom API services are **optional**. Enable them explicitly in the `api:` block:

```yaml
api:
  custom_services: true
  encryption:
    key: !secret api_encryption_key
```

Without `api:` (or with `custom_services: false`), everything above still works from ESPHome YAML. With them enabled, these services are exposed (ESPHome assigns the exact node prefix; `service_prefix` changes the local `ps2_*` portion):

| Service | Parameter | Description |
| --- | --- | --- |
| `esphome.<node>_ps2_type` | `text` | Queue US-ASCII text |
| `esphome.<node>_ps2_stroke` | `key` | Queue a key stroke |
| `esphome.<node>_ps2_press` | `key` | Queue a key-down scan code |
| `esphome.<node>_ps2_release` | `key` | Queue a key-up scan code |
| `esphome.<node>_ps2_combination` | `keys` | Queue a combination such as `CTRL+ALT+DELETE` |
| `esphome.<node>_ps2_send_raw` | `hex_bytes` | Queue raw hexadecimal bytes |

Notes:

- These are fire-and-forget callbacks: ESPHome's custom-service API has no result response, so queue failures are logged on the device rather than returned to Home Assistant. `send_raw` accepts whitespace-, comma-, or semicolon-separated hex (e.g., `E0 75 E0 F0 75`), up to 4096 decoded bytes.
- For an unencrypted development setup you can omit `encryption` — but do not run an unencrypted API on an untrusted network, and never commit your API key or Wi-Fi password.

## Key names

Names are case-insensitive, and common aliases are accepted (`CTRL`/`LCTRL`, `SHIFT`/`LSHIFT`, `WIN`/`GUI`, `DEL`/`DELETE`, `PGDN`/`PAGE_DOWN`).

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

## Supported environment

| Item | Status |
| --- | --- |
| ESP32-S3 | Supported target |
| Other ESP32 variants | Compile-time compatible; verify pin and core support for the board |
| Frameworks | ESP-IDF and Arduino, build-tested with ESPHome 2026.9.0 |
| Scan code set | Set 2 |
| Keyboard layout | US ASCII |
| Unicode/layout switching | Not implemented |

## Limitations and queue behavior

The component implements the PS/2 command/status subset needed by common PC BIOS and OS keyboard controllers. It does **not** implement typematic key repeat, scan code sets other than Set 2, dynamic layouts, or Unicode composition. The power-on BAT (`0xAA`) is scheduled against monotonic setup time: delayed to ~500 ms when setup is early, sent immediately when setup is already late.

**Queue and memory.** The output queue holds up to 16 complete transactions against a 64 KiB memory budget shared by all instances. An estimated allocation is reserved before buffers are built, and admission also checks current free heap and its largest free block, so large requests are rejected early rather than starving the heap. Text is limited to 1024 bytes and raw input to 4096 bytes per transaction. A full queue, disabled reporting, or a memory/heap rejection drops the action with a warning log; callers are never blocked waiting for bus space.

**Retransmission.** A byte that cannot be sent within 500 ms is retried. If a logical scan chunk is inhibited after a clock edge has started, the complete make/break sequence is retransmitted (print/combination chunks correspond to individual make/break sequences). `send_raw` is grouped into fixed 16-byte chunks, but only the incomplete frame is retried so already-delivered bytes are not duplicated. A packet unavailable for 10 seconds is dropped, output is disabled, and a successfully acknowledged host command is required to recover the bus. The same 10-second watchdog covers a bus that stays non-idle with no packet in flight.

**Host commands.** Any host command other than `0xFE` (resend) discards pending output transactions and rejects actions submitted mid-exchange, so stale keystrokes are not sent after a host state change. A command received while waiting for an argument supersedes the pending exchange (per the PS/2 protocol); `0xFE` during that wait resends the last non-`0xFE` byte and leaves the argument exchange pending. Delayed arguments are waited for — an idle bus is not treated as an invalid argument. Per-key `0xFB`/`0xFC`/`0xFD` lists are consumed and acknowledged, but their Set-3-only behavior is ignored. The permissive Set-3 argument profile accepts `0x01..0x7E` plus common `0x83..0x87` and `0x8B..0x8D` extensions; vendor values such as the `0x80` prefix are not treated as standalone make codes. Multi-byte command responses are retransmitted from the start if a later frame is inhibited. Host commands are serviced during inter-byte and hold delays, not after a long action delay finishes.

**Delivery.** All bus timing runs in the component task. The queue protects the ESPHome loop from clocking delays, but delivery cannot be guaranteed while the host holds the bus or has disabled reporting. On a safe ESPHome reboot, the task is signalled out of idle/argument waits, its exit is synchronized, and queued jobs are released before the queue and mutex are destroyed.

## Development and verification

Run the native parser/scancode, transaction, and schema-helper tests plus the ESPHome schema check:

```sh
./script/test
```

The script requires a C++17 compiler and the `esphome` command. A complete firmware check:

```sh
esphome config example.yaml
esphome config tests/api-disabled.yaml
esphome config tests/multi-instance.yaml
esphome compile example.yaml
```

For release qualification, test both `esp-idf` and `arduino`, and use a logic analyzer or protocol-aware harness to verify:

1. The power-on `0xAA` response and reset `0xFF` transcript.
2. Odd parity and stop bits on device-to-host frames.
3. Host LED commands, ACK/resend behavior, and the `F2` `AB 83` response replay policy.
4. Commands received while an argument is pending, including reset/enable replacing the pending command, delayed arguments, and `0xFB`/`0xFC`/`0xFD` list termination.
5. Make/break sequences for normal, extended, Print Screen, and Pause keys, including retransmission after clock inhibition.
6. Cold boot in both a legacy BIOS and a UEFI environment.
7. Bus behavior when the host holds CLK low, sends malformed frames, or disables reporting.
8. Maximum-size text/raw queue stress while Wi-Fi and the API are active, recording current free heap, largest free block, and minimum free heap.
9. Stack high-water marks for the PS/2 task during reset, argument exchanges, and maximum-rate output.

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
