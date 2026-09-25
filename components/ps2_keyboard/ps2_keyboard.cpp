#include "ps2_keyboard.h"
#include "esphome/core/log.h"
#include <rom/ets_sys.h>

namespace esphome {
namespace ps2_keyboard {

static const char *const TAG = "ps2_keyboard";

static const uint32_t CLK_HALF_PERIOD_US = 35;
static const uint32_t CLK_QUARTER_PERIOD_US = 18;
static const uint32_t BYTE_INTERVAL_US = 250;

PS2Keyboard::PS2Keyboard() = default;

void PS2Keyboard::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PS/2 Keyboard...");

  this->clk_pin_->setup();
  this->clk_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN | gpio::FLAG_PULLUP);
  this->clk_pin_->digital_write(true);

  this->data_pin_->setup();
  this->data_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN | gpio::FLAG_PULLUP);
  this->data_pin_->digital_write(true);

  this->isr_clk_ = this->clk_pin_->to_isr();
  this->isr_data_ = this->data_pin_->to_isr();

  this->send_queue_ = xQueueCreate(64, sizeof(PS2Packet));
  if (this->send_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create PS/2 send queue!");
    this->mark_failed();
    return;
  }

  BaseType_t core = (this->task_core_ >= 0) ? this->task_core_ : tskNO_AFFINITY;
  BaseType_t res = xTaskCreatePinnedToCore(
      PS2Keyboard::task_fn,
      "ps2_kb_task",
      4096,
      this,
      this->task_priority_,
      &this->task_handle_,
      core
  );

  if (res != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PS/2 background task!");
    this->mark_failed();
    return;
  }

#ifdef USE_API
  this->register_service(&PS2Keyboard::ha_type, "ps2_type", {"text"});
  this->register_service(&PS2Keyboard::ha_stroke, "ps2_stroke", {"key"});
  this->register_service(&PS2Keyboard::ha_press, "ps2_press", {"key"});
  this->register_service(&PS2Keyboard::ha_release, "ps2_release", {"key"});
  this->register_service(&PS2Keyboard::ha_combination, "ps2_combination", {"keys"});
  this->register_service(&PS2Keyboard::ha_send_raw, "ps2_send_raw", {"hex_bytes"});
#endif

  // Enqueue initial power-on BAT completion code (0xAA)
  PS2Packet bat_packet;
  bat_packet.len = 1;
  bat_packet.data[0] = 0xAA;
  bat_packet.delay_after_ms = 100;
  xQueueSend(this->send_queue_, &bat_packet, 0);

  ESP_LOGI(TAG, "PS/2 Keyboard initialized successfully");
}

void PS2Keyboard::dump_config() {
  ESP_LOGCONFIG(TAG, "PS/2 Keyboard:");
  LOG_PIN("  Clock Pin: ", this->clk_pin_);
  LOG_PIN("  Data Pin: ", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Task Priority: %u", this->task_priority_);
  ESP_LOGCONFIG(TAG, "  Task Core: %d", (int)this->task_core_);
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Caps Lock Sensor", this->caps_lock_sensor_);
  LOG_BINARY_SENSOR("  ", "Num Lock Sensor", this->num_lock_sensor_);
  LOG_BINARY_SENSOR("  ", "Scroll Lock Sensor", this->scroll_lock_sensor_);
#endif
}

void PS2Keyboard::loop() {
  if (this->led_state_changed_) {
    this->led_state_changed_ = false;
    bool caps = this->caps_lock_state_;
    bool num = this->num_lock_state_;
    bool scroll = this->scroll_lock_state_;

    ESP_LOGD(TAG, "LED state updated - Caps: %d, Num: %d, Scroll: %d", caps, num, scroll);

#ifdef USE_BINARY_SENSOR
    if (this->caps_lock_sensor_ != nullptr)
      this->caps_lock_sensor_->publish_state(caps);
    if (this->num_lock_sensor_ != nullptr)
      this->num_lock_sensor_->publish_state(num);
    if (this->scroll_lock_sensor_ != nullptr)
      this->scroll_lock_sensor_->publish_state(scroll);
#endif

    this->led_change_callback_.call(caps, num, scroll);
  }

  if (this->host_reset_detected_) {
    this->host_reset_detected_ = false;
    ESP_LOGD(TAG, "Notifying host reset trigger");
    this->host_reset_callback_.call();
  }
}

void PS2Keyboard::press_key(Key key) {
  if (key == Key::KEY_NONE) return;
  PS2Packet pkt;
  if (get_make_code(key, pkt.data, pkt.len)) {
    pkt.delay_after_ms = 5;
    xQueueSend(this->send_queue_, &pkt, pdMS_TO_TICKS(100));
  }
}

void PS2Keyboard::press_key(const std::string &key_name) {
  Key k = key_from_string(key_name);
  if (k != Key::KEY_NONE) {
    this->press_key(k);
  } else {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
  }
}

void PS2Keyboard::release_key(Key key) {
  if (key == Key::KEY_NONE) return;
  PS2Packet pkt;
  if (get_break_code(key, pkt.data, pkt.len)) {
    pkt.delay_after_ms = 5;
    xQueueSend(this->send_queue_, &pkt, pdMS_TO_TICKS(100));
  }
}

void PS2Keyboard::release_key(const std::string &key_name) {
  Key k = key_from_string(key_name);
  if (k != Key::KEY_NONE) {
    this->release_key(k);
  } else {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
  }
}

void PS2Keyboard::stroke_key(Key key, uint32_t delay_ms) {
  if (key == Key::KEY_NONE) return;
  this->press_key(key);
  if (delay_ms > 0) {
    PS2Packet pause_pkt;
    pause_pkt.len = 0;
    pause_pkt.delay_after_ms = delay_ms;
    xQueueSend(this->send_queue_, &pause_pkt, pdMS_TO_TICKS(100));
  }
  this->release_key(key);
}

void PS2Keyboard::stroke_key(const std::string &key_name, uint32_t delay_ms) {
  Key k = key_from_string(key_name);
  if (k != Key::KEY_NONE) {
    this->stroke_key(k, delay_ms);
  } else {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
  }
}

void PS2Keyboard::press_combination(const std::vector<Key> &keys, uint32_t hold_delay_ms) {
  if (keys.empty()) return;
  for (auto k : keys) {
    this->press_key(k);
  }
  if (hold_delay_ms > 0) {
    PS2Packet pause_pkt;
    pause_pkt.len = 0;
    pause_pkt.delay_after_ms = hold_delay_ms;
    xQueueSend(this->send_queue_, &pause_pkt, pdMS_TO_TICKS(100));
  }
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    this->release_key(*it);
  }
}

void PS2Keyboard::press_combination(const std::string &combo_str, uint32_t hold_delay_ms) {
  auto keys = parse_key_combination(combo_str);
  if (!keys.empty()) {
    this->press_combination(keys, hold_delay_ms);
  } else {
    ESP_LOGW(TAG, "No valid keys found in combination: '%s'", combo_str.c_str());
  }
}

void PS2Keyboard::print(const std::string &text, uint32_t inter_key_delay_ms) {
  for (char c : text) {
    Key key = Key::KEY_NONE;
    bool shift = false;
    if (ascii_to_key(c, key, shift)) {
      if (shift) {
        this->press_key(Key::KEY_LSHIFT);
      }
      this->press_key(key);
      this->release_key(key);
      if (shift) {
        this->release_key(Key::KEY_LSHIFT);
      }
      if (inter_key_delay_ms > 0) {
        PS2Packet pause_pkt;
        pause_pkt.len = 0;
        pause_pkt.delay_after_ms = inter_key_delay_ms;
        xQueueSend(this->send_queue_, &pause_pkt, pdMS_TO_TICKS(100));
      }
    }
  }
}

void PS2Keyboard::send_raw_bytes(const std::vector<uint8_t> &bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    PS2Packet pkt;
    size_t chunk = std::min(bytes.size() - offset, sizeof(pkt.data));
    pkt.len = chunk;
    for (size_t i = 0; i < chunk; i++) {
      pkt.data[i] = bytes[offset + i];
    }
    pkt.delay_after_ms = 5;
    xQueueSend(this->send_queue_, &pkt, pdMS_TO_TICKS(100));
    offset += chunk;
  }
}

#ifdef USE_API
void PS2Keyboard::ha_type(std::string text) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_type: '%s'", text.c_str());
  this->print(text);
}

void PS2Keyboard::ha_stroke(std::string key) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_stroke: '%s'", key.c_str());
  this->stroke_key(key);
}

void PS2Keyboard::ha_press(std::string key) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_press: '%s'", key.c_str());
  this->press_key(key);
}

void PS2Keyboard::ha_release(std::string key) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_release: '%s'", key.c_str());
  this->release_key(key);
}

void PS2Keyboard::ha_combination(std::string keys) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_combination: '%s'", keys.c_str());
  this->press_combination(keys);
}

void PS2Keyboard::ha_send_raw(std::string hex_bytes) {
  ESP_LOGI(TAG, "Home Assistant Action ps2_send_raw: '%s'", hex_bytes.c_str());
  auto bytes = parse_hex_string(hex_bytes);
  if (!bytes.empty()) {
    this->send_raw_bytes(bytes);
  }
}
#endif

bool PS2Keyboard::write_byte(uint8_t data) {
  // If host is inhibiting (CLK=0) or requesting to send (DATA=0), cannot transmit
  if (!this->isr_clk_.digital_read() || !this->isr_data_.digital_read()) {
    return false;
  }

  uint8_t parity = 1;
  for (uint8_t d = data; d; d >>= 1) {
    if (d & 1) parity ^= 1;
  }

  // 11 bits: Start(0), 8 Data bits (LSB first), Parity, Stop(1)
  uint16_t frame = (0 << 0) | ((uint16_t)data << 1) | ((uint16_t)parity << 9) | ((uint16_t)1 << 10);

  InterruptLock lock;

  for (uint8_t bit = 0; bit < 11; bit++) {
    bool bit_val = (frame >> bit) & 1;

    // 1. Set DATA line
    this->isr_data_.digital_write(bit_val);
    ets_delay_us(CLK_QUARTER_PERIOD_US);

    // 2. Pull CLK low
    this->isr_clk_.digital_write(false);
    ets_delay_us(CLK_HALF_PERIOD_US);

    // 3. Release CLK high
    this->isr_clk_.digital_write(true);
    ets_delay_us(CLK_QUARTER_PERIOD_US);

    // If host inhibits before bit 10, abort transmission
    if (bit < 10 && !this->isr_clk_.digital_read()) {
      this->isr_data_.digital_write(true);
      return false;
    }
  }

  // Release data line
  this->isr_data_.digital_write(true);
  this->last_sent_byte_ = data;
  return true;
}

bool PS2Keyboard::write_byte_wait_idle(uint8_t data, uint32_t timeout_us) {
  uint32_t start = micros();
  while (true) {
    // If host RTS is active, abort so caller can handle host request
    if (!this->isr_data_.digital_read() && this->isr_clk_.digital_read()) {
      return false;
    }

    if (this->isr_clk_.digital_read() && this->isr_data_.digital_read()) {
      if (this->write_byte(data)) {
        return true;
      }
    }

    if (micros() - start > timeout_us) {
      return false;
    }
    ets_delay_us(50);
  }
}

bool PS2Keyboard::read_byte(uint8_t *result, uint32_t timeout_us) {
  uint32_t start_time = micros();
  // Wait until bus is in Host RTS state (CLK high, DATA low)
  while (!(!this->isr_data_.digital_read() && this->isr_clk_.digital_read())) {
    if (micros() - start_time > timeout_us) {
      return false;
    }
    delayMicroseconds(10);
  }

  InterruptLock lock;

  // 1. Clock the start bit (host is holding DATA low)
  ets_delay_us(CLK_QUARTER_PERIOD_US);
  this->isr_clk_.digital_write(false);
  ets_delay_us(CLK_HALF_PERIOD_US);
  this->isr_clk_.digital_write(true);
  ets_delay_us(CLK_QUARTER_PERIOD_US);

  // 2. Read 8 data bits (LSB first)
  uint8_t val = 0;
  uint8_t calculated_parity = 1;

  for (uint8_t i = 0; i < 8; i++) {
    ets_delay_us(CLK_QUARTER_PERIOD_US);
    this->isr_clk_.digital_write(false);
    ets_delay_us(CLK_HALF_PERIOD_US);

    bool bit = this->isr_data_.digital_read();
    if (bit) {
      val |= (1 << i);
      calculated_parity ^= 1;
    }

    this->isr_clk_.digital_write(true);
    ets_delay_us(CLK_QUARTER_PERIOD_US);
  }

  // 3. Read parity bit
  ets_delay_us(CLK_QUARTER_PERIOD_US);
  this->isr_clk_.digital_write(false);
  ets_delay_us(CLK_HALF_PERIOD_US);
  bool received_parity = this->isr_data_.digital_read();
  this->isr_clk_.digital_write(true);
  ets_delay_us(CLK_QUARTER_PERIOD_US);

  // 4. Read stop bit (host releases DATA high)
  ets_delay_us(CLK_QUARTER_PERIOD_US);
  this->isr_clk_.digital_write(false);
  ets_delay_us(CLK_HALF_PERIOD_US);
  this->isr_clk_.digital_write(true);
  ets_delay_us(CLK_QUARTER_PERIOD_US);

  // 5. Send ACK bit (device pulls DATA low for one clock cycle)
  this->isr_data_.digital_write(false);
  ets_delay_us(CLK_QUARTER_PERIOD_US);
  this->isr_clk_.digital_write(false);
  ets_delay_us(CLK_HALF_PERIOD_US);
  this->isr_clk_.digital_write(true);
  ets_delay_us(CLK_QUARTER_PERIOD_US);
  this->isr_data_.digital_write(true); // Release DATA

  if (calculated_parity != (received_parity ? 1 : 0)) {
    ESP_LOGW(TAG, "PS/2 Read: Parity error (got %d, expected %d)", (int)received_parity, (int)calculated_parity);
    return false;
  }

  *result = val;
  return true;
}

void PS2Keyboard::send_ack() {
  ets_delay_us(BYTE_INTERVAL_US);
  this->write_byte(0xFA);
}

void PS2Keyboard::handle_host_command(uint8_t cmd) {
  switch (cmd) {
    case 0xFF: // Reset
      ESP_LOGD(TAG, "Host Command: Reset (0xFF)");
      this->send_ack();
      vTaskDelay(pdMS_TO_TICKS(300));
      this->write_byte(0xAA); // BAT Passed
      this->caps_lock_state_ = false;
      this->num_lock_state_ = false;
      this->scroll_lock_state_ = false;
      this->led_state_changed_ = true;
      this->host_reset_detected_ = true;
      this->reporting_enabled_ = true;
      break;

    case 0xFE: // Resend
      ESP_LOGD(TAG, "Host Command: Resend (0xFE)");
      this->write_byte(this->last_sent_byte_);
      break;

    case 0xF6: // Set Defaults
      ESP_LOGD(TAG, "Host Command: Set Defaults (0xF6)");
      this->send_ack();
      this->reporting_enabled_ = true;
      break;

    case 0xF5: // Disable Data Reporting
      ESP_LOGD(TAG, "Host Command: Disable Data Reporting (0xF5)");
      this->reporting_enabled_ = false;
      this->send_ack();
      break;

    case 0xF4: // Enable Data Reporting
      ESP_LOGD(TAG, "Host Command: Enable Data Reporting (0xF4)");
      this->reporting_enabled_ = true;
      this->send_ack();
      break;

    case 0xF3: { // Set Typematic Rate/Delay
      ESP_LOGD(TAG, "Host Command: Set Typematic Rate (0xF3)");
      this->send_ack();
      uint8_t rate_val = 0;
      if (this->read_byte(&rate_val, 50000)) {
        this->send_ack();
      }
      break;
    }

    case 0xF2: // Get Device ID
      ESP_LOGD(TAG, "Host Command: Get Device ID (0xF2)");
      this->send_ack();
      ets_delay_us(BYTE_INTERVAL_US);
      this->write_byte(0xAB);
      ets_delay_us(BYTE_INTERVAL_US);
      this->write_byte(0x83);
      break;

    case 0xF0: { // Set Scan Code Set
      ESP_LOGD(TAG, "Host Command: Set Scan Code Set (0xF0)");
      this->send_ack();
      uint8_t sc_set = 0;
      if (this->read_byte(&sc_set, 50000)) {
        this->send_ack();
        if (sc_set == 0) { // Query
          ets_delay_us(BYTE_INTERVAL_US);
          this->write_byte(0x02); // Standard Set 2
        }
      }
      break;
    }

    case 0xEE: // Echo
      ESP_LOGD(TAG, "Host Command: Echo (0xEE)");
      ets_delay_us(BYTE_INTERVAL_US);
      this->write_byte(0xEE);
      break;

    case 0xED: { // Set/Reset LEDs
      ESP_LOGD(TAG, "Host Command: Set/Reset LEDs (0xED)");
      this->send_ack();
      uint8_t led_val = 0;
      if (this->read_byte(&led_val, 50000)) {
        this->send_ack();
        bool scroll = (led_val & 0x01) != 0;
        bool num    = (led_val & 0x02) != 0;
        bool caps   = (led_val & 0x04) != 0;
        if (scroll != this->scroll_lock_state_ ||
            num != this->num_lock_state_ ||
            caps != this->caps_lock_state_) {
          this->scroll_lock_state_ = scroll;
          this->num_lock_state_ = num;
          this->caps_lock_state_ = caps;
          this->led_state_changed_ = true;
        }
      }
      break;
    }

    default:
      ESP_LOGW(TAG, "Host Command: Unknown 0x%02X", cmd);
      this->send_ack();
      break;
  }
}

void PS2Keyboard::task_fn(void *arg) {
  auto *kb = reinterpret_cast<PS2Keyboard *>(arg);
  kb->run_task();
  vTaskDelete(nullptr);
}

void PS2Keyboard::run_task() {
  vTaskDelay(pdMS_TO_TICKS(200));

  while (true) {
    // 1. Check if host requests to send (CLK=1, DATA=0)
    if (!this->isr_data_.digital_read() && this->isr_clk_.digital_read()) {
      uint8_t cmd = 0;
      if (this->read_byte(&cmd, 50000)) {
        this->handle_host_command(cmd);
      }
      continue;
    }

    // 2. Check if host is inhibiting communication (Host holds CLK=0)
    if (!this->isr_clk_.digital_read()) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // 3. Receive next packet with 5ms timeout to periodically poll for host RTS
    PS2Packet packet;
    if (xQueueReceive(this->send_queue_, &packet, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (packet.len > 0 && this->reporting_enabled_) {
        for (uint8_t i = 0; i < packet.len; i++) {
          while (!this->write_byte_wait_idle(packet.data[i], 20000)) {
            // Check if write was interrupted by host RTS
            if (!this->isr_data_.digital_read() && this->isr_clk_.digital_read()) {
              uint8_t cmd = 0;
              if (this->read_byte(&cmd, 50000)) {
                this->handle_host_command(cmd);
              }
            }
            vTaskDelay(pdMS_TO_TICKS(2));
          }
          ets_delay_us(BYTE_INTERVAL_US);
        }
      }
      if (packet.delay_after_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(packet.delay_after_ms));
      }
    }
  }
}

}  // namespace ps2_keyboard
}  // namespace esphome
