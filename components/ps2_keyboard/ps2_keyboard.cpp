#include "ps2_keyboard.h"

#include <algorithm>
#include <new>
#include <utility>
#include <soc/soc_caps.h>

namespace esphome {
namespace ps2_keyboard {

static const char *const TAG = "ps2_keyboard";

static constexpr uint32_t CLK_HALF_PERIOD_US = 35;
static constexpr uint32_t CLK_HIGH_PERIOD_US = 35;
static constexpr uint32_t CLK_QUARTER_PERIOD_US = 18;
static constexpr uint32_t BYTE_INTERVAL_US = 250;
static constexpr uint32_t HOST_COMMAND_TIMEOUT_US = 50000;
static constexpr uint32_t RESPONSE_TIMEOUT_US = 50000;
static constexpr uint32_t BYTE_SEND_RETRY_INTERVAL_MS = 500;
static constexpr uint32_t HOST_POLL_SLICE_MS = 10;
static constexpr uint32_t RESET_DELAY_MS = 300;
static constexpr uint32_t INITIAL_BAT_DELAY_MS = 100;
static constexpr uint16_t JOB_QUEUE_LENGTH = 16;
static constexpr uint32_t TASK_STACK_WORDS = 4096;
static constexpr size_t MAX_JOB_PACKETS = 4096;
static constexpr size_t MAX_PRINT_BYTES = 1024;
static constexpr size_t MAX_RAW_BYTES = 4096;
static constexpr size_t MAX_COMBINATION_KEYS = 32;

static constexpr uint8_t PS2_ACK = 0xFA;
static constexpr uint8_t PS2_BAT_PASS = 0xAA;
static constexpr uint8_t PS2_RESEND = 0xFE;

static constexpr uint8_t LED_SCROLL = 1 << 0;
static constexpr uint8_t LED_NUM = 1 << 1;
static constexpr uint8_t LED_CAPS = 1 << 2;

struct PS2Job {
  PS2Job(std::vector<PS2Packet> &&packets, uint32_t generation) : packets(std::move(packets)), generation(generation) {}
  std::vector<PS2Packet> packets;
  uint32_t generation;
};

static bool append_scan_code_to_packet(PS2Packet &packet, Key key, bool release) {
  uint8_t sequence[16]{};
  uint8_t new_length = 0;
  const bool ok = release ? get_break_code(key, sequence, new_length) : get_make_code(key, sequence, new_length);
  if (!ok || new_length > sizeof(packet.data) - packet.len)
    return false;
  for (uint8_t i = 0; i < new_length; i++)
    packet.data[packet.len + i] = sequence[i];
  packet.len = static_cast<uint8_t>(packet.len + new_length);
  return true;
}

static bool append_scan_code(std::vector<PS2Packet> &packets, Key key, bool release, uint32_t delay_after_ms) {
  PS2Packet packet{};
  if (!append_scan_code_to_packet(packet, key, release))
    return false;
  packet.delay_after_ms = delay_after_ms;
  packets.push_back(packet);
  return true;
}

static void append_delay(std::vector<PS2Packet> &packets, uint32_t delay_after_ms) {
  if (delay_after_ms == 0)
    return;
  PS2Packet packet{};
  packet.delay_after_ms = delay_after_ms;
  packets.push_back(packet);
}

PS2Keyboard::PS2Keyboard() = default;

void PS2Keyboard::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PS/2 Keyboard...");

  if (this->clk_pin_ == nullptr || this->data_pin_ == nullptr) {
    ESP_LOGE(TAG, "Clock and data pins must be configured");
    this->mark_failed();
    return;
  }

  // The PS/2 bus is open drain. Start with both lines released so reset and
  // boot-time pull states cannot accidentally hold a host keyboard line low.
  this->clk_pin_->setup();
  this->clk_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN | gpio::FLAG_PULLUP);
  this->clk_pin_->digital_write(true);

  this->data_pin_->setup();
  this->data_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_OUTPUT | gpio::FLAG_OPEN_DRAIN | gpio::FLAG_PULLUP);
  this->data_pin_->digital_write(true);

  this->isr_clk_ = this->clk_pin_->to_isr();
  this->isr_data_ = this->data_pin_->to_isr();

  this->job_queue_ = xQueueCreate(JOB_QUEUE_LENGTH, sizeof(PS2Job *));
  if (this->job_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create PS/2 job queue");
    this->mark_failed();
    return;
  }
  if (static_cast<int>(this->task_core_) >= static_cast<int>(SOC_CPU_CORES_NUM)) {
    ESP_LOGE(TAG, "task_core %d is invalid for this ESP32 variant (%d cores available)", static_cast<int>(this->task_core_),
             SOC_CPU_CORES_NUM);
    vQueueDelete(this->job_queue_);
    this->job_queue_ = nullptr;
    this->mark_failed();
    return;
  }

  const BaseType_t core = this->task_core_ >= 0 ? static_cast<BaseType_t>(this->task_core_) : tskNO_AFFINITY;
  const BaseType_t result = xTaskCreatePinnedToCore(PS2Keyboard::task_fn, "ps2_kb_task", TASK_STACK_WORDS, this,
                                                   this->task_priority_, &this->task_handle_, core);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PS/2 background task");
    vQueueDelete(this->job_queue_);
    this->job_queue_ = nullptr;
    this->mark_failed();
    return;
  }

#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
  this->register_service(&PS2Keyboard::ha_type, this->service_prefix_ + "_type", {"text"});
  this->register_service(&PS2Keyboard::ha_stroke, this->service_prefix_ + "_stroke", {"key"});
  this->register_service(&PS2Keyboard::ha_press, this->service_prefix_ + "_press", {"key"});
  this->register_service(&PS2Keyboard::ha_release, this->service_prefix_ + "_release", {"key"});
  this->register_service(&PS2Keyboard::ha_combination, this->service_prefix_ + "_combination", {"keys"});
  this->register_service(&PS2Keyboard::ha_send_raw, this->service_prefix_ + "_send_raw", {"hex_bytes"});
#endif

  std::vector<PS2Packet> initial_packets;
  PS2Packet bat_packet{};
  bat_packet.len = 1;
  bat_packet.data[0] = PS2_BAT_PASS;
  bat_packet.delay_after_ms = INITIAL_BAT_DELAY_MS;
  initial_packets.push_back(bat_packet);
  this->enqueue_job(std::move(initial_packets));

  if (!this->bus_idle()) {
    ESP_LOGW(TAG, "PS/2 bus is not idle at setup (CLK=%s DATA=%s)", this->isr_clk_.digital_read() ? "HIGH" : "LOW",
             this->isr_data_.digital_read() ? "HIGH" : "LOW");
  }
  ESP_LOGI(TAG, "PS/2 Keyboard initialized");
}

void PS2Keyboard::dump_config() {
  ESP_LOGCONFIG(TAG, "PS/2 Keyboard:");
  LOG_PIN("  Clock Pin: ", this->clk_pin_);
  LOG_PIN("  Data Pin: ", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Task Priority: %u", this->task_priority_);
  ESP_LOGCONFIG(TAG, "  Task Core: %d", static_cast<int>(this->task_core_));
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Caps Lock Sensor", this->caps_lock_sensor_);
  LOG_BINARY_SENSOR("  ", "Num Lock Sensor", this->num_lock_sensor_);
  LOG_BINARY_SENSOR("  ", "Scroll Lock Sensor", this->scroll_lock_sensor_);
#endif
}

void PS2Keyboard::loop() {
  const bool initial_led_state = !this->led_state_initialized_;
  if (initial_led_state || this->led_state_changed_.exchange(false)) {
    const uint8_t state = this->led_state_.load();
    const bool caps = (state & LED_CAPS) != 0;
    const bool num = (state & LED_NUM) != 0;
    const bool scroll = (state & LED_SCROLL) != 0;

#ifdef USE_BINARY_SENSOR
    if (this->caps_lock_sensor_ != nullptr)
      this->caps_lock_sensor_->publish_state(caps);
    if (this->num_lock_sensor_ != nullptr)
      this->num_lock_sensor_->publish_state(num);
    if (this->scroll_lock_sensor_ != nullptr)
      this->scroll_lock_sensor_->publish_state(scroll);
#endif

    this->led_state_initialized_ = true;
    if (!initial_led_state) {
      ESP_LOGD(TAG, "LED state updated - Caps: %d, Num: %d, Scroll: %d", caps, num, scroll);
      this->led_change_callback_.call(caps, num, scroll);
    }
  }

  if (this->host_reset_detected_.exchange(false)) {
    ESP_LOGD(TAG, "Host reset command received");
    this->host_reset_callback_.call();
  }
}

void PS2Keyboard::press_key(Key key) {
  if (key == Key::KEY_NONE)
    return;
  std::vector<PS2Packet> packets;
  if (!append_scan_code(packets, key, false, 5)) {
    ESP_LOGW(TAG, "No make scan code for key");
    return;
  }
  this->enqueue_job(std::move(packets));
}

void PS2Keyboard::press_key(const std::string &key_name) {
  const Key key = key_from_string(key_name);
  if (key == Key::KEY_NONE) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->press_key(key);
}

void PS2Keyboard::release_key(Key key) {
  if (key == Key::KEY_NONE)
    return;
  std::vector<PS2Packet> packets;
  if (!append_scan_code(packets, key, true, 5)) {
    ESP_LOGW(TAG, "No break scan code for key");
    return;
  }
  this->enqueue_job(std::move(packets));
}

void PS2Keyboard::release_key(const std::string &key_name) {
  const Key key = key_from_string(key_name);
  if (key == Key::KEY_NONE) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->release_key(key);
}

void PS2Keyboard::stroke_key(Key key, uint32_t delay_ms) {
  if (key == Key::KEY_NONE)
    return;
  std::vector<PS2Packet> packets;
  if (!append_scan_code(packets, key, false, delay_ms) || !append_scan_code(packets, key, true, 5)) {
    ESP_LOGW(TAG, "No complete scan sequence for key");
    return;
  }
  this->enqueue_job(std::move(packets));
}

void PS2Keyboard::stroke_key(const std::string &key_name, uint32_t delay_ms) {
  const Key key = key_from_string(key_name);
  if (key == Key::KEY_NONE) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->stroke_key(key, delay_ms);
}

void PS2Keyboard::print(const std::string &text, uint32_t inter_key_delay_ms) {
  if (text.empty())
    return;
  if (text.size() > MAX_PRINT_BYTES) {
    ESP_LOGW(TAG, "Text action is too large (%u bytes; maximum is %u)", static_cast<unsigned>(text.size()),
             static_cast<unsigned>(MAX_PRINT_BYTES));
    return;
  }

  std::vector<PS2Packet> packets;
  packets.reserve(text.size() * 2);
  uint32_t unsupported = 0;
  for (char c : text) {
    Key key = Key::KEY_NONE;
    bool shift = false;
    if (!ascii_to_key(c, key, shift)) {
      unsupported++;
      continue;
    }

    PS2Packet character_packet{};
    bool valid = true;
    if (shift)
      valid = append_scan_code_to_packet(character_packet, Key::KEY_LSHIFT, false);
    if (valid)
      valid = append_scan_code_to_packet(character_packet, key, false);
    if (valid)
      valid = append_scan_code_to_packet(character_packet, key, true);
    if (valid && shift)
      valid = append_scan_code_to_packet(character_packet, Key::KEY_LSHIFT, true);
    if (!valid) {
      unsupported++;
      continue;
    }
    character_packet.delay_after_ms = 5;
    packets.push_back(character_packet);
    append_delay(packets, inter_key_delay_ms);
  }

  if (unsupported != 0) {
    ESP_LOGW(TAG, "Ignored %u unsupported character(s) while printing", static_cast<unsigned>(unsupported));
  }
  if (!packets.empty())
    this->enqueue_job(std::move(packets));
}

void PS2Keyboard::press_combination(const std::vector<Key> &keys, uint32_t hold_delay_ms) {
  if (keys.empty() || keys.size() > MAX_COMBINATION_KEYS) {
    ESP_LOGW(TAG, "Combination must contain between 1 and %u keys", static_cast<unsigned>(MAX_COMBINATION_KEYS));
    return;
  }
  for (Key key : keys) {
    if (key == Key::KEY_NONE) {
      ESP_LOGW(TAG, "Combination contains an unknown key");
      return;
    }
  }

  std::vector<PS2Packet> packets;
  packets.reserve(keys.size() * 2 + 1);
  for (Key key : keys) {
    if (!append_scan_code(packets, key, false, 5))
      return;
  }
  append_delay(packets, hold_delay_ms);
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    if (!append_scan_code(packets, *it, true, 5))
      return;
  }
  this->enqueue_job(std::move(packets));
}

void PS2Keyboard::press_combination(const std::string &combo_str, uint32_t hold_delay_ms) {
  std::vector<Key> keys;
  if (!parse_key_combination(combo_str, keys) || keys.empty()) {
    ESP_LOGW(TAG, "Invalid key combination: '%s'", combo_str.c_str());
    return;
  }
  this->press_combination(keys, hold_delay_ms);
}

void PS2Keyboard::send_raw_bytes(const std::vector<uint8_t> &bytes) {
  if (bytes.empty())
    return;
  if (bytes.size() > MAX_RAW_BYTES) {
    ESP_LOGW(TAG, "Raw action is too large (%u bytes; maximum is %u)", static_cast<unsigned>(bytes.size()),
             static_cast<unsigned>(MAX_RAW_BYTES));
    return;
  }

  std::vector<PS2Packet> packets;
  packets.reserve((bytes.size() + sizeof(PS2Packet{}.data) - 1) / sizeof(PS2Packet{}.data));
  for (size_t offset = 0; offset < bytes.size();) {
    PS2Packet packet{};
    const size_t chunk = std::min(bytes.size() - offset, sizeof(packet.data));
    packet.len = static_cast<uint8_t>(chunk);
    std::copy(bytes.begin() + offset, bytes.begin() + offset + chunk, packet.data);
    packet.delay_after_ms = 5;
    packets.push_back(packet);
    offset += chunk;
  }
  this->enqueue_job(std::move(packets));
}

#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
void PS2Keyboard::ha_type(std::string text) {
  ESP_LOGI(TAG, "Home Assistant action ps2_type received (%u characters)", static_cast<unsigned>(text.size()));
  this->print(text);
}

void PS2Keyboard::ha_stroke(std::string key) {
  ESP_LOGI(TAG, "Home Assistant action ps2_stroke received");
  this->stroke_key(key);
}

void PS2Keyboard::ha_press(std::string key) {
  ESP_LOGI(TAG, "Home Assistant action ps2_press received");
  this->press_key(key);
}

void PS2Keyboard::ha_release(std::string key) {
  ESP_LOGI(TAG, "Home Assistant action ps2_release received");
  this->release_key(key);
}

void PS2Keyboard::ha_combination(std::string keys) {
  ESP_LOGI(TAG, "Home Assistant action ps2_combination received");
  this->press_combination(keys);
}

void PS2Keyboard::ha_send_raw(std::string hex_bytes) {
  ESP_LOGI(TAG, "Home Assistant action ps2_send_raw received (%u characters)", static_cast<unsigned>(hex_bytes.size()));
  const auto bytes = parse_hex_string(hex_bytes);
  if (bytes.empty()) {
    ESP_LOGW(TAG, "Raw byte list is empty or invalid");
    return;
  }
  this->send_raw_bytes(bytes);
}
#endif

bool PS2Keyboard::bus_idle() {
  return this->isr_clk_.digital_read() && this->isr_data_.digital_read();
}

bool PS2Keyboard::host_request_pending() {
  return !this->isr_data_.digital_read() && this->isr_clk_.digital_read();
}

bool PS2Keyboard::enqueue_job(std::vector<PS2Packet> &&packets) {
  if (packets.empty())
    return true;
  if (packets.size() > MAX_JOB_PACKETS) {
    ESP_LOGW(TAG, "Output job contains too many packets");
    return false;
  }
  if (this->job_queue_ == nullptr) {
    ESP_LOGE(TAG, "Cannot queue output before the PS/2 task is initialized");
    return false;
  }
  if (this->reset_in_progress_.load()) {
    ESP_LOGW(TAG, "Output action rejected while the keyboard is resetting");
    return false;
  }
  if (!this->reporting_enabled_.load()) {
    ESP_LOGW(TAG, "Output action rejected while data reporting is disabled by the host");
    return false;
  }

  const uint32_t generation = this->output_generation_.load();
  auto *job = new (std::nothrow) PS2Job(std::move(packets), generation);
  if (job == nullptr) {
    ESP_LOGE(TAG, "Unable to allocate PS/2 output job");
    return false;
  }
  if (xQueueSend(this->job_queue_, &job, 0) != pdTRUE) {
    delete job;
    ESP_LOGW(TAG, "PS/2 output queue is full; action rejected");
    return false;
  }
  return true;
}

void PS2Keyboard::drop_pending_jobs() {
  if (this->job_queue_ == nullptr)
    return;
  const uint32_t current_generation = this->output_generation_.load();
  void *raw_job = nullptr;
  uint16_t dropped = 0;
  while (xQueueReceive(this->job_queue_, &raw_job, 0) == pdTRUE) {
    auto *job = static_cast<PS2Job *>(raw_job);
    if (job->generation == current_generation) {
      // Preserve actions queued after the generation change. There is one
      // free queue slot while the item is being reinserted.
      if (xQueueSend(this->job_queue_, &raw_job, 0) == pdTRUE) {
        continue;
      }
    }
    delete job;
    raw_job = nullptr;
    if (dropped != UINT16_MAX)
      dropped++;
  }
  if (dropped != 0) {
    ESP_LOGD(TAG, "Dropped %u stale PS/2 output job(s)", static_cast<unsigned>(dropped));
  }
}

void PS2Keyboard::invalidate_output_jobs() {
  this->output_generation_.fetch_add(1);
  this->drop_pending_jobs();
}

bool PS2Keyboard::write_byte(uint8_t data) {
  if (!this->bus_idle())
    return false;

  // Start (0), eight data bits (LSB first), odd parity, stop (1).
  const uint16_t frame = static_cast<uint16_t>((static_cast<uint16_t>(data) << 1) |
                                                  (static_cast<uint16_t>(ps2_odd_parity(data)) << 9) | (1U << 10));
  InterruptLock lock;

  for (uint8_t bit = 0; bit < 11; bit++) {
    // A host request can arrive between bytes. Do not contend with the host
    // while it owns DATA or has pulled CLK low.
    if (!this->bus_idle()) {
      this->isr_data_.digital_write(true);
      return false;
    }

    this->isr_data_.digital_write((frame >> bit) & 1);
    delayMicroseconds(CLK_QUARTER_PERIOD_US);
    if (!this->isr_clk_.digital_read()) {
      this->isr_data_.digital_write(true);
      return false;
    }
    this->isr_clk_.digital_write(false);
    delayMicroseconds(CLK_HALF_PERIOD_US);
    this->isr_clk_.digital_write(true);
    delayMicroseconds(CLK_QUARTER_PERIOD_US);
    // Release DATA during the high phase. This leaves the bus idle between
    // bits and lets us distinguish our own zero bit from a host request.
    this->isr_data_.digital_write(true);
  }

  this->isr_data_.digital_write(true);
  this->last_sent_byte_.store(data);
  return true;
}

bool PS2Keyboard::write_byte_wait_idle(uint8_t data, uint32_t timeout_us) {
  const uint32_t start = micros();
  while (true) {
    if (this->host_request_pending())
      return false;
    if (this->bus_idle() && this->write_byte(data))
      return true;
    if (static_cast<uint32_t>(micros() - start) > timeout_us)
      return false;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool PS2Keyboard::read_byte(uint8_t *result, uint32_t timeout_us) {
  const uint32_t start_time = micros();
  while (!this->host_request_pending()) {
    if (static_cast<uint32_t>(micros() - start_time) > timeout_us) {
      this->isr_data_.digital_write(true);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  bool start_bit = false;
  bool parity_ok = false;
  bool stop_bit = false;
  uint8_t value = 0;
  {
    InterruptLock lock;
    this->isr_data_.digital_write(true);
    delayMicroseconds(CLK_QUARTER_PERIOD_US);
    this->isr_clk_.digital_write(false);
    delayMicroseconds(CLK_HALF_PERIOD_US);
    start_bit = !this->isr_data_.digital_read();
    this->isr_clk_.digital_write(true);
    delayMicroseconds(CLK_HIGH_PERIOD_US);

    uint8_t expected_parity = 1;
    for (uint8_t i = 0; i < 8; i++) {
      this->isr_clk_.digital_write(false);
      delayMicroseconds(CLK_HALF_PERIOD_US);
      const bool bit = this->isr_data_.digital_read();
      if (bit) {
        value |= static_cast<uint8_t>(1U << i);
        expected_parity ^= 1;
      }
      this->isr_clk_.digital_write(true);
      delayMicroseconds(CLK_HIGH_PERIOD_US);
    }

    this->isr_clk_.digital_write(false);
    delayMicroseconds(CLK_HALF_PERIOD_US);
    const bool received_parity = this->isr_data_.digital_read();
    this->isr_clk_.digital_write(true);
    delayMicroseconds(CLK_HIGH_PERIOD_US);

    this->isr_clk_.digital_write(false);
    delayMicroseconds(CLK_HALF_PERIOD_US);
    stop_bit = this->isr_data_.digital_read();
    this->isr_clk_.digital_write(true);
    delayMicroseconds(CLK_HIGH_PERIOD_US);

    parity_ok = received_parity == (expected_parity != 0);
    if (start_bit && parity_ok && stop_bit) {
      // ACK is a low DATA bit for one clock period.
      this->isr_data_.digital_write(false);
      delayMicroseconds(CLK_QUARTER_PERIOD_US);
      this->isr_clk_.digital_write(false);
      delayMicroseconds(CLK_HALF_PERIOD_US);
      this->isr_clk_.digital_write(true);
      delayMicroseconds(CLK_HIGH_PERIOD_US);
      this->isr_data_.digital_write(true);
      *result = value;
      return true;
    }
    if (start_bit) {
      // The receiver still owns the ACK clock position for a bad frame; leave
      // DATA high to indicate that the frame was not acknowledged.
      this->isr_clk_.digital_write(false);
      delayMicroseconds(CLK_HALF_PERIOD_US);
      this->isr_clk_.digital_write(true);
      delayMicroseconds(CLK_HIGH_PERIOD_US);
    }
  }

  this->isr_data_.digital_write(true);
  if (!start_bit) {
    ESP_LOGW(TAG, "PS/2 frame error: start bit was not low");
  }
  if (!parity_ok) {
    ESP_LOGW(TAG, "PS/2 frame error: parity bit was invalid");
  }
  if (!stop_bit) {
    ESP_LOGW(TAG, "PS/2 frame error: stop bit was not high");
  }
  return false;
}

bool PS2Keyboard::write_response_byte(uint8_t data) {
  delayMicroseconds(BYTE_INTERVAL_US);
  if (this->write_byte_wait_idle(data, RESPONSE_TIMEOUT_US))
    return true;
  ESP_LOGW(TAG, "Unable to send PS/2 response 0x%02X", data);
  return false;
}

bool PS2Keyboard::send_ack() {
  return this->write_response_byte(PS2_ACK);
}

void PS2Keyboard::send_resend_request() {
  this->write_response_byte(PS2_RESEND);
}

bool PS2Keyboard::handle_host_request() {
  if (!this->host_request_pending())
    return false;
  uint8_t command = 0;
  if (!this->read_byte(&command, HOST_COMMAND_TIMEOUT_US)) {
    this->send_resend_request();
    return false;
  }
  return this->handle_host_command(command);
}

bool PS2Keyboard::handle_host_command(uint8_t cmd) {
  switch (cmd) {
    case 0xFF: {  // Reset
      ESP_LOGD(TAG, "Host command: reset");
      this->reset_in_progress_.store(true);
      this->reporting_enabled_.store(false);
      this->invalidate_output_jobs();
      this->send_ack();
      vTaskDelay(pdMS_TO_TICKS(RESET_DELAY_MS));
      this->reset_device_state();
      this->reporting_enabled_.store(true);
      this->write_response_byte(PS2_BAT_PASS);
      this->invalidate_output_jobs();
      this->host_reset_detected_.store(true);
      this->reset_in_progress_.store(false);
      return true;  // discard the output job interrupted by the reset
    }

    case 0xFA:  // Set all keys to generate break and typematic codes
      ESP_LOGD(TAG, "Host command: enable breaks and typematic for all keys");
      this->reporting_enabled_.store(true);
      this->send_ack();
      return false;

    case 0xFE:  // Host requests the last byte again.
      ESP_LOGD(TAG, "Host command: resend");
      this->write_response_byte(this->last_sent_byte_.load());
      return false;

    case 0xF6: {  // Set defaults
      ESP_LOGD(TAG, "Host command: set defaults");
      this->reporting_enabled_.store(true);
      this->reset_device_state();
      this->invalidate_output_jobs();
      this->send_ack();
      return true;
    }

    case 0xF5:  // Disable data reporting
      ESP_LOGD(TAG, "Host command: disable reporting");
      this->reporting_enabled_.store(false);
      this->invalidate_output_jobs();
      this->send_ack();
      return true;

    case 0xF4:  // Enable data reporting
      ESP_LOGD(TAG, "Host command: enable reporting");
      this->reporting_enabled_.store(true);
      this->send_ack();
      return false;

    case 0xF3: {  // Set typematic rate/delay
      ESP_LOGD(TAG, "Host command: set typematic parameters");
      if (!this->send_ack())
        return false;
      uint8_t value = 0;
      if (!this->read_byte(&value, HOST_COMMAND_TIMEOUT_US)) {
        this->send_resend_request();
        return false;
      }
      if (value > 0x7F) {
        this->send_resend_request();
        return false;
      }
      this->send_ack();
      return false;
    }

    case 0xF2:  // Get device ID
      ESP_LOGD(TAG, "Host command: get device ID");
      if (this->send_ack() && this->write_response_byte(0xAB)) {
        this->write_response_byte(0x83);
      }
      return false;

    case 0xF0: {  // Set/query scan code set
      ESP_LOGD(TAG, "Host command: set scan code set");
      if (!this->send_ack())
        return false;
      uint8_t scan_set = 0;
      if (!this->read_byte(&scan_set, HOST_COMMAND_TIMEOUT_US)) {
        this->send_resend_request();
        return false;
      }
      if (!this->send_ack())
        return false;
      if (scan_set == 0) {
        this->write_response_byte(0x02);
      } else if (scan_set != 0x02) {
        ESP_LOGW(TAG, "Host requested unsupported scan code set 0x%02X", scan_set);
        this->send_resend_request();
      }
      return false;
    }

    case 0xEE:  // Echo
      ESP_LOGD(TAG, "Host command: echo");
      this->write_response_byte(0xEE);
      return false;

    case 0xED: {  // Set/reset LEDs
      ESP_LOGD(TAG, "Host command: set LEDs");
      if (!this->send_ack())
        return false;
      uint8_t leds = 0;
      if (!this->read_byte(&leds, HOST_COMMAND_TIMEOUT_US)) {
        this->send_resend_request();
        return false;
      }
      if (!this->send_ack())
        return false;
      this->set_led_state((leds & LED_CAPS) != 0, (leds & LED_NUM) != 0, (leds & LED_SCROLL) != 0);
      return false;
    }

    case 0xF7:
    case 0xF8:
    case 0xF9:
    case 0xFB:
    case 0xFC:
    case 0xFD:  // Per-key typematic/break parameter commands
      ESP_LOGD(TAG, "Host command: set keyboard parameter mode");
      // This emulator does not generate typematic repeats or alter break
      // generation, but it acknowledges the command so controller probing can
      // continue. Per-key parameter lists are not interpreted as scan codes.
      this->send_ack();
      return false;

    default:
      ESP_LOGW(TAG, "Host command: unknown 0x%02X", cmd);
      this->send_resend_request();
      return false;
  }
}

bool PS2Keyboard::delay_ms_interruptible(uint32_t delay_ms) {
  uint32_t remaining = delay_ms;
  while (remaining != 0) {
    if (this->host_request_pending() && this->handle_host_request())
      return true;
    if (this->reset_in_progress_.load())
      return true;

    TickType_t requested_ticks = pdMS_TO_TICKS(remaining);
    if (requested_ticks == 0)
      requested_ticks = 1;
    TickType_t max_ticks = pdMS_TO_TICKS(HOST_POLL_SLICE_MS);
    if (max_ticks == 0)
      max_ticks = 1;
    const TickType_t ticks = std::min(requested_ticks, max_ticks);
    vTaskDelay(ticks);
    const uint32_t elapsed = static_cast<uint32_t>(ticks) * portTICK_PERIOD_MS;
    remaining = elapsed >= remaining ? 0 : remaining - elapsed;
  }
  return false;
}

PS2Keyboard::ByteSendResult PS2Keyboard::send_queued_byte(uint8_t data) {
  const uint32_t start = millis();
  while (true) {
    if (!this->reporting_enabled_.load())
      return ByteSendResult::ABORTED;
    if (this->host_request_pending() && this->handle_host_request())
      return ByteSendResult::ABORTED;
    if (this->bus_idle() && this->write_byte(data))
      return ByteSendResult::SENT;
    if (static_cast<uint32_t>(millis() - start) >= BYTE_SEND_RETRY_INTERVAL_MS)
      return ByteSendResult::TIMED_OUT;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void PS2Keyboard::set_led_state(bool caps, bool num, bool scroll) {
  const uint8_t next = static_cast<uint8_t>((caps ? LED_CAPS : 0) | (num ? LED_NUM : 0) | (scroll ? LED_SCROLL : 0));
  uint8_t current = this->led_state_.load();
  while (current != next && !this->led_state_.compare_exchange_weak(current, next)) {
  }
  if (current != next)
    this->led_state_changed_.store(true);
}

void PS2Keyboard::reset_device_state() {
  this->led_state_.store(0);
  this->led_state_changed_.store(true);
  this->reporting_enabled_.store(true);
}

void PS2Keyboard::process_job(PS2Job *job) {
  if (job == nullptr)
    return;

  for (const PS2Packet &packet : job->packets) {
    if (packet.len == 0) {
      if (this->delay_ms_interruptible(packet.delay_after_ms))
        return;
      continue;
    }
    if (!this->reporting_enabled_.load())
      return;

    uint8_t i = 0;
    while (i < packet.len) {
      const ByteSendResult result = this->send_queued_byte(packet.data[i]);
      if (result == ByteSendResult::ABORTED)
        return;
      if (result == ByteSendResult::TIMED_OUT) {
        ESP_LOGW(TAG, "PS/2 output timed out; retrying byte %u", static_cast<unsigned>(i));
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      delayMicroseconds(BYTE_INTERVAL_US);
      i++;
    }
    if (this->delay_ms_interruptible(packet.delay_after_ms))
      return;
  }
}

void PS2Keyboard::task_fn(void *arg) {
  auto *keyboard = reinterpret_cast<PS2Keyboard *>(arg);
  keyboard->run_task();
  vTaskDelete(nullptr);
}

void PS2Keyboard::run_task() {
  // Give the host and the rest of ESPHome time to finish boot before sending
  // the power-on self-test result.
  vTaskDelay(pdMS_TO_TICKS(200));

  while (true) {
    if (this->host_request_pending()) {
      this->handle_host_request();
      continue;
    }
    if (!this->bus_idle()) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    void *raw_job = nullptr;
    if (xQueueReceive(this->job_queue_, &raw_job, pdMS_TO_TICKS(5)) == pdTRUE) {
      auto *job = static_cast<PS2Job *>(raw_job);
      if (job->generation == this->output_generation_.load()) {
        this->process_job(job);
      }
      delete job;
    }
  }
}

}  // namespace ps2_keyboard
}  // namespace esphome
