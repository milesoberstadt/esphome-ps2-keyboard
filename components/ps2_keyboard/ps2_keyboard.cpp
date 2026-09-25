#include "ps2_keyboard.h"
#include "memory.h"
#include "transactions.h"

#include <algorithm>
#include <new>
#include <utility>
#include <soc/soc_caps.h>
#ifdef USE_ESP32
#include <esp_heap_caps.h>
#endif

namespace esphome {
namespace ps2_keyboard {

static const char *const TAG = "ps2_keyboard";

static constexpr uint32_t CLK_HALF_PERIOD_US = 35;
static constexpr uint32_t CLK_HIGH_PERIOD_US = 35;
static constexpr uint32_t CLK_QUARTER_PERIOD_US = 18;
static constexpr uint32_t CLK_IDLE_GUARD_US = 50;
static constexpr uint32_t BYTE_INTERVAL_US = 250;
static constexpr uint32_t HOST_COMMAND_TIMEOUT_US = 50000;
static constexpr uint32_t RESPONSE_TIMEOUT_US = 50000;
static constexpr uint32_t BYTE_SEND_RETRY_INTERVAL_MS = 500;
static constexpr uint32_t PACKET_SEND_TIMEOUT_MS = 10000;
static constexpr uint32_t HOST_POLL_SLICE_MS = 1;
static constexpr uint32_t RESET_DELAY_MS = 500;

// Some valid ESP32 FreeRTOS configurations use a 100 Hz tick. In that case
// pdMS_TO_TICKS(1) rounds down to zero; always give a delay loop at least one
// tick so it cannot become a tight polling loop.
static TickType_t rtos_delay_ticks(uint32_t delay_ms) {
  TickType_t ticks = pdMS_TO_TICKS(delay_ms);
  return ticks == 0 ? 1 : ticks;
}
static constexpr uint32_t INITIAL_BAT_DELAY_MS = 500;
static constexpr uint16_t JOB_QUEUE_LENGTH = 16;
// ESP-IDF's FreeRTOS API (including the ESPHome Arduino integration) expresses
// xTaskCreate stack depth in bytes.
static constexpr uint32_t TASK_STACK_BYTES = 8192;
// This budget is shared by all PS/2 instances in the firmware. A per-instance
// budget would allow a multi-instance configuration to multiply the peak heap
// use without bound.
static constexpr size_t MAX_OUTSTANDING_JOB_BYTES = 64 * 1024;
static constexpr size_t TRANSACTION_ALLOCATION_HEADROOM = 16 * 1024;
static std::atomic<size_t> committed_output_bytes_{0};

static constexpr uint8_t PS2_ACK = 0xFA;
static constexpr uint8_t PS2_BAT_PASS = 0xAA;
static constexpr uint8_t PS2_RESEND = 0xFE;

static constexpr uint8_t LED_SCROLL = 1 << 0;
static constexpr uint8_t LED_NUM = 1 << 1;
static constexpr uint8_t LED_CAPS = 1 << 2;

static bool is_host_command(uint8_t value) {
  switch (value) {
    case 0xED:
    case 0xEE:
    case 0xF0:
    case 0xF2:
    case 0xF3:
    case 0xF4:
    case 0xF5:
    case 0xF6:
    case 0xF7:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
    case 0xFF:
      return true;
    default:
      return false;
  }
}

struct PS2Job {
  PS2Job(PS2Transaction &&transaction, uint32_t generation)
      : transaction(std::move(transaction)), generation(generation) {}

  size_t allocation_bytes() const {
    return sizeof(PS2Job) + this->transaction.data.capacity() * sizeof(uint8_t) +
           this->transaction.packets.capacity() * sizeof(PS2PacketMeta);
  }

  PS2Transaction transaction;
  uint32_t generation;
};

static bool transaction_memory_available(size_t data_bytes, size_t packet_count) {
  size_t packet_bytes = 0;
  if (!memory_detail::checked_multiply(packet_count, sizeof(PS2PacketMeta), packet_bytes))
    return false;
#ifdef USE_ESP32
  const memory_detail::HeapSnapshot heap{
      heap_caps_get_free_size(MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
  };
  return memory_detail::transaction_fits(heap, sizeof(PS2Job), data_bytes, packet_bytes,
                                         TRANSACTION_ALLOCATION_HEADROOM);
#else
  // Native builds do not model the ESP32 heap. The transaction limits and the
  // shared committed-memory budget still apply above this platform check.
  (void) data_bytes;
  (void) packet_count;
  (void) packet_bytes;
  return true;
#endif
}

static size_t transaction_reservation_bytes(size_t data_bytes, size_t packet_count) {
  size_t packet_bytes = 0;
  size_t result = 0;
  if (!memory_detail::checked_multiply(packet_count, sizeof(PS2PacketMeta), packet_bytes) ||
      !memory_detail::checked_add(sizeof(PS2Job), data_bytes, result) ||
      !memory_detail::checked_add(result, packet_bytes, result))
    return 0;
  return result;
}

static bool commit_output_reservation(size_t reserved, size_t actual) {
  return memory_detail::replace_reservation(committed_output_bytes_, MAX_OUTSTANDING_JOB_BYTES, reserved, actual);
}

PS2Keyboard::PS2Keyboard() = default;

size_t PS2Keyboard::reserve_output_memory(size_t data_bytes, size_t packet_count) {
  size_t packet_bytes = 0;
  if (!memory_detail::checked_multiply(packet_count, sizeof(PS2PacketMeta), packet_bytes) ||
      !transaction_memory_available(data_bytes, packet_count))
    return 0;
  const size_t reservation = transaction_reservation_bytes(data_bytes, packet_count);
  if (reservation == 0 || reservation > MAX_OUTSTANDING_JOB_BYTES ||
      !memory_detail::reserve(committed_output_bytes_, MAX_OUTSTANDING_JOB_BYTES, reservation))
    return 0;
  return reservation;
}

void PS2Keyboard::release_output_memory(size_t reservation) {
  if (reservation == 0)
    return;
  if (!memory_detail::release(committed_output_bytes_, reservation)) {
    // This indicates an accounting bug, not a recoverable allocation
    // condition. Do not underflow the shared counter; fail closed so a later
    // action cannot make the overrun less visible.
    ESP_LOGE(TAG, "PS/2 output memory accounting underflow (requested %u bytes, committed %u)",
             static_cast<unsigned>(reservation), static_cast<unsigned>(committed_output_bytes_.load()));
  }
}

void PS2Keyboard::cleanup_runtime_resources() {
  // This is called only before the task starts or after it has signalled that
  // it has stopped. It is therefore safe to drain and destroy the queue here.
  if (this->job_queue_ != nullptr) {
    if (this->queue_mutex_ != nullptr) {
      xSemaphoreTake(this->queue_mutex_, portMAX_DELAY);
      this->drop_pending_jobs_locked();
      xSemaphoreGive(this->queue_mutex_);
    } else {
      this->drop_pending_jobs_locked();
    }
    vQueueDelete(this->job_queue_);
    this->job_queue_ = nullptr;
  }
  if (this->queue_mutex_ != nullptr) {
    vSemaphoreDelete(this->queue_mutex_);
    this->queue_mutex_ = nullptr;
  }
  if (this->task_exit_semaphore_ != nullptr) {
    vSemaphoreDelete(this->task_exit_semaphore_);
    this->task_exit_semaphore_ = nullptr;
  }
  this->task_handle_ = nullptr;
}

void PS2Keyboard::on_shutdown() {
  this->shutdown_requested_.store(true);
  // Invalidate queued work immediately. The task owns the currently running
  // job and will destroy it after it observes the shutdown flag.
  this->invalidate_output_jobs();
}

bool PS2Keyboard::teardown() {
  if (this->task_handle_ == nullptr) {
    this->cleanup_runtime_resources();
    return true;
  }
  if (this->task_exit_semaphore_ == nullptr)
    return false;
  // The task gives this semaphore immediately before deleting itself. Polling
  // with a zero timeout lets ESPHome's teardown scheduler feed its watchdog.
  if (xSemaphoreTake(this->task_exit_semaphore_, 0) != pdTRUE)
    return false;
  this->cleanup_runtime_resources();
  return true;
}

void PS2Keyboard::setup() {
  ESP_LOGCONFIG(TAG, "Setting up PS/2 Keyboard...");
  this->shutdown_requested_.store(false);

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

  this->queue_mutex_ = xSemaphoreCreateMutex();
  if (this->queue_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create PS/2 queue mutex");
    this->mark_failed();
    return;
  }

  this->job_queue_ = xQueueCreate(JOB_QUEUE_LENGTH, sizeof(PS2Job *));
  if (this->job_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create PS/2 job queue");
    vSemaphoreDelete(this->queue_mutex_);
    this->queue_mutex_ = nullptr;
    this->mark_failed();
    return;
  }
  if (static_cast<int>(this->task_core_) >= static_cast<int>(SOC_CPU_CORES_NUM)) {
    ESP_LOGE(TAG, "task_core %d is invalid for this ESP32 variant (%d cores available)", static_cast<int>(this->task_core_),
             SOC_CPU_CORES_NUM);
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }

  // Queue the power-on BAT before the bus task can service a host request.
  // Otherwise a command received during setup could invalidate the BAT and
  // then allow a newly queued BAT to inherit the post-command generation.
  const size_t bat_reservation = this->reserve_output_memory(1, 1);
  if (bat_reservation == 0) {
    ESP_LOGE(TAG, "Insufficient heap for the PS/2 power-on BAT response");
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }
  // The task is created after ESPHome has completed much of its own boot
  // sequence. Aim for the 500--750 ms power-on BAT window when setup time is
  // known, and send immediately if setup was already late rather than adding
  // another full delay.
  const uint32_t setup_elapsed_ms = millis();
  this->initial_bat_delay_ms_ =
      setup_elapsed_ms < INITIAL_BAT_DELAY_MS ? INITIAL_BAT_DELAY_MS - setup_elapsed_ms : 0;

  PS2Transaction initial_transaction;
  if (!initial_transaction.data.reserve(1) || !initial_transaction.packets.reserve(1)) {
    this->release_output_memory(bat_reservation);
    ESP_LOGE(TAG, "Unable to allocate the PS/2 power-on BAT buffers");
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }
  PS2Packet bat_packet{};
  bat_packet.len = 1;
  bat_packet.data[0] = PS2_BAT_PASS;
  bat_packet.retransmit_on_abort = true;
  if (!append_packet(initial_transaction, bat_packet)) {
    this->release_output_memory(bat_reservation);
    ESP_LOGE(TAG, "Unable to build the PS/2 power-on BAT response");
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }
  if (!this->enqueue_job(std::move(initial_transaction), bat_reservation)) {
    ESP_LOGE(TAG, "Unable to queue the PS/2 power-on BAT response");
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }

  this->task_exit_semaphore_ = xSemaphoreCreateBinary();
  if (this->task_exit_semaphore_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create the PS/2 task-exit semaphore");
    this->cleanup_runtime_resources();
    this->mark_failed();
    return;
  }

  const BaseType_t core = this->task_core_ >= 0 ? static_cast<BaseType_t>(this->task_core_) : tskNO_AFFINITY;
  const BaseType_t result = xTaskCreatePinnedToCore(PS2Keyboard::task_fn, "ps2_kb_task", TASK_STACK_BYTES, this,
                                                   this->task_priority_, &this->task_handle_, core);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PS/2 background task");
    this->task_handle_ = nullptr;
    this->cleanup_runtime_resources();
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
  this->press_key_with_shift(key, false);
}

void PS2Keyboard::press_key(const std::string &key_name) {
  Key key = Key::KEY_NONE;
  bool shift = false;
  if (!key_from_string_with_shift(key_name, key, shift)) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->press_key_with_shift(key, shift);
}

void PS2Keyboard::press_key_with_shift(Key key, bool shift) {
  if (key == Key::KEY_NONE)
    return;
  const size_t packet_count = shift ? 2 : 1;
  const size_t reservation = this->reserve_output_memory(16, packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 key action");
    return;
  }
  PS2Transaction transaction;
  if (!transaction.data.reserve(16) || !transaction.packets.reserve(packet_count)) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Unable to allocate PS/2 key-action buffers");
    return;
  }
  bool valid = true;
  if (shift)
    valid = append_scan_code(transaction, Key::KEY_LSHIFT, false, 0);
  if (valid)
    valid = append_scan_code(transaction, key, false, 5);
  if (!valid) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "No make scan code for key");
    return;
  }
  this->enqueue_job(std::move(transaction), reservation);
}

void PS2Keyboard::release_key(Key key) {
  this->release_key_with_shift(key, false);
}

void PS2Keyboard::release_key(const std::string &key_name) {
  Key key = Key::KEY_NONE;
  bool shift = false;
  if (!key_from_string_with_shift(key_name, key, shift)) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->release_key_with_shift(key, shift);
}

void PS2Keyboard::release_key_with_shift(Key key, bool shift) {
  if (key == Key::KEY_NONE)
    return;
  const size_t packet_count = shift ? 2 : 1;
  const size_t reservation = this->reserve_output_memory(16, packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 key action");
    return;
  }
  PS2Transaction transaction;
  if (!transaction.data.reserve(16) || !transaction.packets.reserve(packet_count)) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Unable to allocate PS/2 key-action buffers");
    return;
  }
  bool valid = append_scan_code(transaction, key, true, shift ? 0 : 5);
  if (valid && shift)
    valid = append_scan_code(transaction, Key::KEY_LSHIFT, true, 5);
  if (!valid) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "No break scan code for key");
    return;
  }
  this->enqueue_job(std::move(transaction), reservation);
}

void PS2Keyboard::stroke_key(Key key, uint32_t delay_ms) {
  this->stroke_key_with_shift(key, false, delay_ms);
}

void PS2Keyboard::stroke_key(const std::string &key_name, uint32_t delay_ms) {
  Key key = Key::KEY_NONE;
  bool shift = false;
  if (!key_from_string_with_shift(key_name, key, shift)) {
    ESP_LOGW(TAG, "Unknown key name: '%s'", key_name.c_str());
    return;
  }
  this->stroke_key_with_shift(key, shift, delay_ms);
}

void PS2Keyboard::stroke_key_with_shift(Key key, bool shift, uint32_t delay_ms) {
  if (key == Key::KEY_NONE)
    return;
  const size_t packet_count = shift ? 4 : 2;
  const size_t reservation = this->reserve_output_memory(16, packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 key action");
    return;
  }
  PS2Transaction transaction;
  if (!transaction.data.reserve(16) || !transaction.packets.reserve(packet_count)) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Unable to allocate PS/2 key-action buffers");
    return;
  }
  bool valid = true;
  if (shift)
    valid = append_scan_code(transaction, Key::KEY_LSHIFT, false, 0);
  if (valid)
    valid = append_scan_code(transaction, key, false, delay_ms);
  if (valid)
    valid = append_scan_code(transaction, key, true, 0);
  if (valid && shift)
    valid = append_scan_code(transaction, Key::KEY_LSHIFT, true, 5);
  if (!valid) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "No complete scan sequence for key");
    return;
  }
  this->enqueue_job(std::move(transaction), reservation);
}

void PS2Keyboard::print(const std::string &text, uint32_t inter_key_delay_ms) {
  if (text.empty())
    return;
  if (text.size() > MAX_PRINT_BYTES) {
    ESP_LOGW(TAG, "Text action is too large (%u bytes; maximum is %u)", static_cast<unsigned>(text.size()),
             static_cast<unsigned>(MAX_PRINT_BYTES));
    return;
  }

  const bool caps_lock_enabled = (this->led_state_.load() & LED_CAPS) != 0;
  const uint32_t character_delay_ms = inter_key_delay_ms > UINT32_MAX - 5 ? UINT32_MAX : inter_key_delay_ms + 5;
  PS2PrintPlan plan;
  uint32_t unsupported = 0;
  if (!plan_print_transaction(text, caps_lock_enabled, plan, unsupported)) {
    ESP_LOGW(TAG, "Text action exceeds the PS/2 transaction limits");
    return;
  }
  const size_t reservation = this->reserve_output_memory(plan.data_bytes, plan.packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 text action");
    return;
  }

  PS2Transaction transaction;
  if (!build_print_transaction(text, caps_lock_enabled, character_delay_ms, plan, transaction, unsupported)) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Unable to build the PS/2 text action");
    return;
  }
  if (unsupported != 0) {
    ESP_LOGW(TAG, "Ignored %u unsupported character(s) while printing", static_cast<unsigned>(unsupported));
  }
  if (transaction.packets.empty()) {
    this->release_output_memory(reservation);
    return;
  }
  this->enqueue_job(std::move(transaction), reservation);
}

void PS2Keyboard::press_combination(const std::vector<Key> &keys, uint32_t hold_delay_ms) {
  if (keys.empty() || keys.size() > MAX_COMBINATION_KEYS) {
    ESP_LOGW(TAG, "Combination must contain between 1 and %u keys", static_cast<unsigned>(MAX_COMBINATION_KEYS));
    return;
  }
  std::vector<KeyCombinationPart> parts;
  parts.reserve(keys.size());
  for (Key key : keys)
    parts.push_back(KeyCombinationPart{key, false});
  this->press_combination(parts, hold_delay_ms);
}

void PS2Keyboard::press_combination(const std::vector<KeyCombinationPart> &keys, uint32_t hold_delay_ms) {
  if (keys.empty() || keys.size() > MAX_COMBINATION_KEYS) {
    ESP_LOGW(TAG, "Combination must contain between 1 and %u keys", static_cast<unsigned>(MAX_COMBINATION_KEYS));
    return;
  }
  size_t data_bytes = 0;
  size_t packet_count = hold_delay_ms == 0 ? 0 : 1;
  for (const KeyCombinationPart &part : keys) {
    if (part.key == Key::KEY_NONE) {
      ESP_LOGW(TAG, "Combination contains an unknown key");
      return;
    }
    size_t make_length = 0;
    size_t break_length = 0;
    if (!scan_code_lengths(part.key, make_length, break_length) ||
        !memory_detail::checked_add(data_bytes, make_length, data_bytes) ||
        !memory_detail::checked_add(data_bytes, break_length, data_bytes)) {
      ESP_LOGW(TAG, "Combination contains a key without a complete scan-code sequence");
      return;
    }
    packet_count += part.shift ? 4 : 2;
    if (part.shift) {
      size_t shift_make_length = 0;
      size_t shift_break_length = 0;
      if (!scan_code_lengths(Key::KEY_LSHIFT, shift_make_length, shift_break_length) ||
          !memory_detail::checked_add(data_bytes, shift_make_length, data_bytes) ||
          !memory_detail::checked_add(data_bytes, shift_break_length, data_bytes)) {
        ESP_LOGW(TAG, "Unable to size the combination Shift sequences");
        return;
      }
    }
  }
  if (packet_count > MAX_JOB_PACKETS || data_bytes > MAX_JOB_DATA_BYTES) {
    ESP_LOGW(TAG, "Combination exceeds the component transaction limits");
    return;
  }
  const size_t reservation = this->reserve_output_memory(data_bytes, packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 combination action");
    return;
  }
  auto release_reservation = [this, reservation]() { this->release_output_memory(reservation); };
  PS2Transaction transaction;
  if (!transaction.packets.reserve(packet_count) || !transaction.data.reserve(data_bytes)) {
    release_reservation();
    ESP_LOGW(TAG, "Unable to allocate PS/2 combination buffers");
    return;
  }
  for (const KeyCombinationPart &part : keys) {
    if (part.shift && !append_scan_code(transaction, Key::KEY_LSHIFT, false, 0)) {
      release_reservation();
      return;
    }
    if (!append_scan_code(transaction, part.key, false, 5)) {
      release_reservation();
      return;
    }
  }
  if (!append_delay(transaction, hold_delay_ms)) {
    release_reservation();
    return;
  }
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    if (!append_scan_code(transaction, it->key, true, it->shift ? 0 : 5)) {
      release_reservation();
      return;
    }
    if (it->shift && !append_scan_code(transaction, Key::KEY_LSHIFT, true, 5)) {
      release_reservation();
      return;
    }
  }
  this->enqueue_job(std::move(transaction), reservation);
}

void PS2Keyboard::press_combination(const std::string &combo_str, uint32_t hold_delay_ms) {
  std::vector<KeyCombinationPart> keys;
  if (!parse_key_combination_with_shift(combo_str, keys) || keys.empty()) {
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

  const size_t packet_count = (bytes.size() + PS2_PACKET_DATA_BYTES - 1) / PS2_PACKET_DATA_BYTES;
  const size_t reservation = this->reserve_output_memory(bytes.size(), packet_count);
  if (reservation == 0) {
    ESP_LOGW(TAG, "Insufficient heap or output memory budget for the PS/2 raw action");
    return;
  }
  PS2Transaction transaction;
  if (!build_raw_transaction(bytes, transaction)) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Raw output transaction exceeds the component limits");
    return;
  }
  this->enqueue_job(std::move(transaction), reservation);
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
  if (hex_bytes.size() > MAX_HEX_INPUT_CHARS) {
    ESP_LOGW(TAG, "Raw byte list is too large (%u characters; maximum is %u)", static_cast<unsigned>(hex_bytes.size()),
             static_cast<unsigned>(MAX_HEX_INPUT_CHARS));
    return;
  }
  const auto bytes = parse_hex_string(hex_bytes, MAX_RAW_BYTES, MAX_HEX_INPUT_CHARS);
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

bool PS2Keyboard::enqueue_job(PS2Transaction &&transaction, size_t reservation) {
  if (reservation == 0) {
    ESP_LOGW(TAG, "Output job has no memory reservation");
    return false;
  }
  if (transaction.packets.empty()) {
    this->release_output_memory(reservation);
    return true;
  }
  if (transaction.data.allocation_failed() || transaction.packets.allocation_failed()) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Output transaction storage allocation failed");
    return false;
  }
  if (transaction.packets.size() > MAX_JOB_PACKETS || transaction.data.size() > MAX_JOB_DATA_BYTES) {
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "Output job exceeds the component packet limits");
    return false;
  }
  if (this->job_queue_ == nullptr || this->queue_mutex_ == nullptr) {
    this->release_output_memory(reservation);
    ESP_LOGE(TAG, "Cannot queue output before the PS/2 task is initialized");
    return false;
  }

  auto *job = new (std::nothrow) PS2Job(std::move(transaction), 0);
  if (job == nullptr) {
    this->release_output_memory(reservation);
    ESP_LOGE(TAG, "Unable to allocate PS/2 output job");
    return false;
  }
  if (xSemaphoreTake(this->queue_mutex_, 0) != pdTRUE) {
    delete job;
    this->release_output_memory(reservation);
    ESP_LOGW(TAG, "PS/2 output queue is busy; action rejected");
    return false;
  }

  bool accepted = false;
  bool reservation_accounted = false;
  if (this->shutdown_requested_.load()) {
    ESP_LOGW(TAG, "Output action rejected while the PS/2 component is shutting down");
  } else if (this->host_command_in_progress_.load()) {
    ESP_LOGW(TAG, "Output action rejected while the host is sending a command");
  } else if (this->reset_in_progress_.load()) {
    ESP_LOGW(TAG, "Output action rejected while the keyboard is resetting");
  } else if (this->bus_faulted_.load()) {
    ESP_LOGW(TAG, "Output action rejected until the host recovers the PS/2 bus");
  } else if (!this->reporting_enabled_.load()) {
    ESP_LOGW(TAG, "Output action rejected while data reporting is disabled by the host");
  } else {
    const size_t job_bytes = job->allocation_bytes();
    if (job_bytes == 0 || job_bytes > MAX_OUTSTANDING_JOB_BYTES ||
        !commit_output_reservation(reservation, job_bytes)) {
      ESP_LOGW(TAG, "PS/2 output memory budget is full; action rejected");
    } else {
      reservation_accounted = true;
      job->generation = this->output_generation_.load();
      if (xQueueSend(this->job_queue_, &job, 0) != pdTRUE) {
        // Destroy the buffers before returning their budget to the shared
        // counter. This keeps the accounting useful under concurrent output.
        delete job;
        job = nullptr;
        this->release_output_memory(job_bytes);
        ESP_LOGW(TAG, "PS/2 output queue is full; action rejected");
      } else {
        accepted = true;
      }
    }
  }
  xSemaphoreGive(this->queue_mutex_);

  if (!accepted) {
    delete job;
    if (!reservation_accounted)
      this->release_output_memory(reservation);
    return false;
  }
  return true;
}

void PS2Keyboard::drop_pending_jobs_locked() {
  if (this->job_queue_ == nullptr)
    return;
  void *raw_job = nullptr;
  uint16_t dropped = 0;
  while (xQueueReceive(this->job_queue_, &raw_job, 0) == pdTRUE) {
    auto *job = static_cast<PS2Job *>(raw_job);
    if (job != nullptr) {
      const size_t job_bytes = job->allocation_bytes();
      delete job;
      this->release_output_memory(job_bytes);
    }
    raw_job = nullptr;
    if (dropped != UINT16_MAX)
      dropped++;
  }
  if (dropped != 0) {
    ESP_LOGD(TAG, "Dropped %u stale PS/2 output job(s)", static_cast<unsigned>(dropped));
  }
}

void PS2Keyboard::release_job_memory(PS2Job *job) {
  if (job == nullptr)
    return;

  // Compute the charge while the buffers are still alive, then destroy the
  // job before returning its bytes to the global budget. The atomic budget
  // does not need the queue mutex, and taking it here would deadlock when
  // called while invalidating a queue from the bus task.
  const size_t job_bytes = job->allocation_bytes();
  delete job;
  this->release_output_memory(job_bytes);
}

void PS2Keyboard::invalidate_output_jobs() {
  if (this->queue_mutex_ == nullptr) {
    this->output_generation_.fetch_add(1);
    this->drop_pending_jobs_locked();
    return;
  }
  xSemaphoreTake(this->queue_mutex_, portMAX_DELAY);
  this->output_generation_.fetch_add(1);
  this->drop_pending_jobs_locked();
  xSemaphoreGive(this->queue_mutex_);
}

void PS2Keyboard::enter_bus_fault() {
  const bool already_faulted = this->bus_faulted_.exchange(true);
  this->reporting_enabled_.store(false);
  this->invalidate_output_jobs();
  if (!already_faulted) {
    ESP_LOGE(TAG, "PS/2 bus remained unavailable; output disabled until a host command recovers it");
  }
}

bool PS2Keyboard::write_byte(uint8_t data, bool *frame_started) {
  if (frame_started != nullptr)
    *frame_started = false;
  if (!this->bus_idle())
    return false;

  // Start (0), eight data bits (LSB first), odd parity, stop (1).
  const uint16_t frame = static_cast<uint16_t>((static_cast<uint16_t>(data) << 1) |
                                                  (static_cast<uint16_t>(ps2_odd_parity(data)) << 9) | (1U << 10));
  bool started = false;
  // The calls made while interrupts are disabled are ESP-IDF's IRAM GPIO and
  // microsecond-delay helpers; keep the critical region limited to one frame.
  InterruptLock lock;

  for (uint8_t bit = 0; bit < 11; bit++) {
    // A host request can arrive between bytes. Do not contend with the host
    // while it owns DATA or has pulled CLK low.
    if (!this->bus_idle()) {
      this->isr_data_.digital_write(true);
      if (frame_started != nullptr)
        *frame_started = started;
      return false;
    }

    this->isr_data_.digital_write((frame >> bit) & 1);
    delayMicroseconds(CLK_QUARTER_PERIOD_US);
    if (!this->isr_clk_.digital_read()) {
      this->isr_data_.digital_write(true);
      if (frame_started != nullptr)
        *frame_started = started;
      return false;
    }
    this->isr_clk_.digital_write(false);
    started = true;
    if (frame_started != nullptr)
      *frame_started = true;
    delayMicroseconds(CLK_HALF_PERIOD_US);
    this->isr_clk_.digital_write(true);
    // The next iteration waits CLK_QUARTER_PERIOD_US before pulling CLK low;
    // keep the complete high cell within the PS/2 30--50 us limit.
    delayMicroseconds(bit == 10 ? CLK_HIGH_PERIOD_US : CLK_HIGH_PERIOD_US - CLK_QUARTER_PERIOD_US);
    if (bit == 10 && !this->isr_clk_.digital_read()) {
      // The host may have inhibited the released stop-bit clock. Do not
      // report a frame as successfully clocked unless its final high phase
      // actually occurred.
      this->isr_data_.digital_write(true);
      if (frame_started != nullptr)
        *frame_started = started;
      return false;
    }
    // Release DATA during the high phase. This leaves the bus idle between
    // bits and lets us distinguish our own zero bit from a host request.
    this->isr_data_.digital_write(true);
  }

  this->isr_data_.digital_write(true);
  if (data != PS2_RESEND)
    this->last_data_byte_.store(data);
  return true;
}

bool PS2Keyboard::write_byte_wait_idle(uint8_t data, uint32_t timeout_us, bool *frame_started) {
  if (frame_started != nullptr)
    *frame_started = false;
  const uint32_t start = micros();
  bool idle = false;
  uint32_t idle_since = 0;
  while (true) {
    if (this->shutdown_requested_.load())
      return false;
    if (this->host_request_pending())
      return false;
    if (this->bus_idle()) {
      const uint32_t now = micros();
      if (!idle) {
        idle = true;
        idle_since = now;
      }
      if (static_cast<uint32_t>(now - idle_since) >= CLK_IDLE_GUARD_US) {
        if (this->write_byte(data, frame_started))
          return true;
        // Do not hide a partial frame behind an internal retry. The caller
        // must be able to restart the complete logical response sequence.
        if (frame_started != nullptr && *frame_started)
          return false;
        idle = false;
      }
    } else {
      idle = false;
    }
    if (static_cast<uint32_t>(micros() - start) > timeout_us)
      return false;
    vTaskDelay(rtos_delay_ticks(1));
  }
}

bool PS2Keyboard::read_byte(uint8_t *result, uint32_t timeout_us, bool *aborted, bool *invalid) {
  if (aborted != nullptr)
    *aborted = false;
  if (invalid != nullptr)
    *invalid = false;

  const uint32_t start_time = micros();
  while (!this->host_request_pending()) {
    if (static_cast<uint32_t>(micros() - start_time) > timeout_us) {
      this->isr_data_.digital_write(true);
      return false;
    }
    vTaskDelay(rtos_delay_ticks(1));
  }

  bool start_bit = false;
  bool parity_ok = false;
  bool stop_bit = false;
  bool frame_aborted = false;
  uint8_t value = 0;
  {
    // As with device-to-host framing, the critical region only uses the
    // IRAM-resident GPIO and timing primitives.
    InterruptLock lock;

    // In host-to-device mode, the host's request-to-send state (DATA low while
    // CLK is high) is the implicit start bit. The device then clocks the eight
    // data bits, parity, and stop bit; the start bit does not get its own
    // clock pulse. Sampling the first clocked bit as a start bit would shift
    // every following bit and make commands with data bit 0 set fail.
    start_bit = !this->isr_data_.digital_read();
    this->isr_data_.digital_write(true);
    delayMicroseconds(CLK_QUARTER_PERIOD_US);

    // The host changes DATA while CLK is low. Sample in the middle of the
    // high phase, after the host has had the full low period to update it.
    auto clock_and_sample = [&](bool &sample) {
      this->isr_clk_.digital_write(false);
      delayMicroseconds(CLK_HALF_PERIOD_US);
      this->isr_clk_.digital_write(true);
      delayMicroseconds(CLK_HIGH_PERIOD_US / 2);
      if (!this->isr_clk_.digital_read()) {
        frame_aborted = true;
        return false;
      }
      sample = this->isr_data_.digital_read();
      delayMicroseconds(CLK_HIGH_PERIOD_US - CLK_HIGH_PERIOD_US / 2);
      if (!this->isr_clk_.digital_read()) {
        frame_aborted = true;
        return false;
      }
      return true;
    };

    uint8_t expected_parity = 1;
    for (uint8_t i = 0; i < 8 && !frame_aborted; i++) {
      bool bit = false;
      if (!clock_and_sample(bit))
        break;
      if (bit) {
        value |= static_cast<uint8_t>(1U << i);
        expected_parity ^= 1;
      }
    }

    bool received_parity = false;
    if (!frame_aborted && !clock_and_sample(received_parity))
      frame_aborted = true;
    if (!frame_aborted && !clock_and_sample(stop_bit))
      frame_aborted = true;

    if (!frame_aborted) {
      parity_ok = received_parity == (expected_parity != 0);
      if (start_bit && parity_ok && stop_bit) {
        // ACK is a low DATA bit for one clock period.
        this->isr_data_.digital_write(false);
        delayMicroseconds(CLK_QUARTER_PERIOD_US);
        this->isr_clk_.digital_write(false);
        delayMicroseconds(CLK_HALF_PERIOD_US);
        this->isr_clk_.digital_write(true);
        delayMicroseconds(CLK_HIGH_PERIOD_US);
        if (!this->isr_clk_.digital_read())
          frame_aborted = true;
        this->isr_data_.digital_write(true);
        if (!frame_aborted) {
          *result = value;
          return true;
        }
      }
      if (start_bit && !frame_aborted) {
        // The receiver still owns the ACK clock position for a bad frame; leave
        // DATA high to indicate that the frame was not acknowledged. Release
        // DATA first because an invalid stop bit may have left it low.
        this->isr_data_.digital_write(true);
        delayMicroseconds(CLK_QUARTER_PERIOD_US);
        this->isr_clk_.digital_write(false);
        delayMicroseconds(CLK_HALF_PERIOD_US);
        this->isr_clk_.digital_write(true);
        delayMicroseconds(CLK_HIGH_PERIOD_US);
        if (!this->isr_clk_.digital_read())
          frame_aborted = true;
      }
    }
  }

  this->isr_clk_.digital_write(true);
  this->isr_data_.digital_write(true);
  if (frame_aborted) {
    if (aborted != nullptr)
      *aborted = true;
    return false;
  }
  if (invalid != nullptr && (!start_bit || !parity_ok || !stop_bit))
    *invalid = true;
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

bool PS2Keyboard::read_host_argument(uint8_t *result) {
  uint32_t bus_non_idle_since = 0;
  uint32_t request_pending_since = 0;
  while (true) {
    if (this->shutdown_requested_.load())
      return false;
    if (this->reset_in_progress_.load())
      return false;

    // An idle bus is a valid state while the host decides when to send an
    // argument; keep this wait pending indefinitely. Only a physically
    // non-idle inhibit or Request-to-Send state is covered by the watchdog.
    const uint32_t now = millis();
    const bool request_pending = this->host_request_pending();
    if (request_pending) {
      if (request_pending_since == 0) {
        request_pending_since = now;
      } else if (!this->bus_faulted_.load() &&
                 static_cast<uint32_t>(now - request_pending_since) >= PACKET_SEND_TIMEOUT_MS) {
        this->enter_bus_fault();
        return false;
      }
      bus_non_idle_since = 0;
    } else {
      request_pending_since = 0;
      if (!this->bus_idle()) {
        if (bus_non_idle_since == 0) {
          bus_non_idle_since = now;
        } else if (!this->bus_faulted_.load() &&
                   static_cast<uint32_t>(now - bus_non_idle_since) >= PACKET_SEND_TIMEOUT_MS) {
          this->enter_bus_fault();
          return false;
        }
      } else {
        bus_non_idle_since = 0;
      }
    }

    if (request_pending) {
      bool aborted = false;
      bool invalid = false;
      if (this->read_byte(result, HOST_COMMAND_TIMEOUT_US, &aborted, &invalid))
        return true;
      if (!aborted && invalid && !this->send_resend_request()) {
        this->enter_bus_fault();
        return false;
      }
      // A host may abort a frame and retry it, or may answer a resend with
      // the argument later. Keep the command gate closed instead of treating
      // an idle bus as an invalid argument.
    }

    vTaskDelay(rtos_delay_ticks(1));
  }
}

bool PS2Keyboard::write_response_bytes(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0)
    return true;

  const uint32_t start = millis();
  size_t index = 0;
  while (index < length) {
    if (this->shutdown_requested_.load())
      return false;
    if (static_cast<uint32_t>(millis() - start) >= PACKET_SEND_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Timed out sending a PS/2 response sequence");
      return false;
    }

    delayMicroseconds(BYTE_INTERVAL_US);
    bool frame_started = false;
    if (this->write_byte_wait_idle(data[index], RESPONSE_TIMEOUT_US, &frame_started)) {
      index++;
      continue;
    }

    if (frame_started) {
      // A response such as AB 83 is one logical device-ID reply. If a later
      // frame is inhibited after clocking began, restart the complete reply
      // so the host observes one coherent response sequence.
      ESP_LOGW(TAG, "PS/2 response sequence interrupted; retransmitting");
      index = 0;
      continue;
    }

    ESP_LOGW(TAG, "Unable to send PS/2 response 0x%02X", data[index]);
    return false;
  }
  return true;
}

bool PS2Keyboard::write_response_byte(uint8_t data) {
  if (this->write_response_bytes(&data, 1))
    return true;
  return false;
}

bool PS2Keyboard::send_ack() {
  return this->write_response_byte(PS2_ACK);
}

bool PS2Keyboard::send_resend_request() {
  return this->write_response_byte(PS2_RESEND);
}

bool PS2Keyboard::handle_host_request() {
  if (!this->host_request_pending())
    return false;
  uint8_t command = 0;
  bool aborted = false;
  if (!this->read_byte(&command, HOST_COMMAND_TIMEOUT_US, &aborted)) {
    if (!aborted && !this->send_resend_request())
      this->enter_bus_fault();
    return false;
  }
  return this->handle_host_command(command);
}

bool PS2Keyboard::handle_host_command(uint8_t cmd) {
  // A host command flushes the keyboard's pending output. Resend is the one
  // command that must remain attached to the current logical transmission.
  // Keep the command-in-progress gate set until the complete command (and any
  // argument/response exchange) has finished, otherwise a producer could queue
  // a new job in the small window between invalidation and the state change.
  bool invalidated_output = false;
  bool command_gate_active = false;
  bool response_ok = true;

  while (true) {
    const bool invalidates_output = cmd != PS2_RESEND;
    if (invalidates_output && !command_gate_active) {
      this->host_command_in_progress_.store(true);
      this->invalidate_output_jobs();
      invalidated_output = true;
      command_gate_active = true;
    }

    uint8_t replacement = cmd;
    const HostCommandResult result = this->handle_host_command_impl(cmd, &replacement);
    if (result == HostCommandResult::REPLACE) {
      // PS/2 commands replace one another while an argument is pending. The
      // output buffer was already invalidated by the first command, and the
      // command gate remains active for the replacement exchange.
      cmd = replacement;
      continue;
    }
    if (result == HostCommandResult::COMPLETE_RESPONSE_FAILED)
      response_ok = false;
    break;
  }

  if (command_gate_active)
    this->host_command_in_progress_.store(false);

  // Do not declare a previously wedged bus healthy until the command's
  // required response was actually clocked successfully. Conversely, a
  // response that could not be clocked is itself a bus fault; otherwise a
  // transiently wedged bus could accept new output after a failed command.
  if (!response_ok)
    this->enter_bus_fault();
  else if (invalidated_output)
    this->bus_faulted_.store(false);
  return invalidated_output;
}

PS2Keyboard::HostCommandResult PS2Keyboard::handle_host_command_impl(uint8_t cmd, uint8_t *replacement) {
  switch (cmd) {
    case 0xFF: {  // Reset
      ESP_LOGD(TAG, "Host command: reset");
      const bool reporting_before_reset = this->reporting_enabled_.load();
      this->reset_in_progress_.store(true);
      this->reporting_enabled_.store(false);
      if (!this->send_ack()) {
        this->reporting_enabled_.store(reporting_before_reset);
        this->reset_in_progress_.store(false);
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      }
      vTaskDelay(rtos_delay_ticks(RESET_DELAY_MS));
      this->reset_device_state();
      this->reporting_enabled_.store(true);
      const bool bat_sent = this->write_response_byte(PS2_BAT_PASS);
      this->host_reset_detected_.store(true);
      this->reset_in_progress_.store(false);
      if (!bat_sent) {
        this->enter_bus_fault();
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      }
      return HostCommandResult::COMPLETE;
    }

    case 0xFA:  // Set all keys to typematic/make/break mode
      ESP_LOGD(TAG, "Host command: set all-key parameter mode");
      return this->send_ack() ? HostCommandResult::COMPLETE : HostCommandResult::COMPLETE_RESPONSE_FAILED;

    case 0xFE:  // Host requests the last byte again.
      ESP_LOGD(TAG, "Host command: resend");
      return this->write_response_byte(this->last_data_byte_.load()) ? HostCommandResult::COMPLETE
                                                                     : HostCommandResult::COMPLETE_RESPONSE_FAILED;

    case 0xF6: {  // Set defaults
      ESP_LOGD(TAG, "Host command: set defaults");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      this->reset_device_state();
      return HostCommandResult::COMPLETE;
    }

    case 0xF5:  // Disable data reporting and load defaults
      ESP_LOGD(TAG, "Host command: disable reporting and load defaults");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      this->reset_device_defaults();
      this->reporting_enabled_.store(false);
      return HostCommandResult::COMPLETE;

    case 0xF4:  // Enable data reporting
      ESP_LOGD(TAG, "Host command: enable reporting");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      this->reporting_enabled_.store(true);
      return HostCommandResult::COMPLETE;

    case 0xF3: {  // Set typematic rate/delay
      ESP_LOGD(TAG, "Host command: set typematic parameters");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      while (true) {
        uint8_t value = 0;
        if (!this->read_host_argument(&value))
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        if (value == PS2_RESEND) {
          if (!this->write_response_byte(this->last_data_byte_.load()))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (is_host_command(value)) {
          *replacement = value;
          return HostCommandResult::REPLACE;
        }
        if (value > 0x7F) {
          if (!this->write_response_byte(PS2_RESEND))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        return this->send_ack() ? HostCommandResult::COMPLETE : HostCommandResult::COMPLETE_RESPONSE_FAILED;
      }
    }

    case 0xF2: {  // Get device ID
      ESP_LOGD(TAG, "Host command: get device ID");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      static const uint8_t keyboard_id[] = {0xAB, 0x83};
      if (!this->write_response_bytes(keyboard_id, sizeof(keyboard_id)))
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      return HostCommandResult::COMPLETE;
    }

    case 0xF0: {  // Set/query scan code set
      ESP_LOGD(TAG, "Host command: set scan code set");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      while (true) {
        uint8_t scan_set = 0;
        if (!this->read_host_argument(&scan_set))
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        if (scan_set == PS2_RESEND) {
          if (!this->write_response_byte(this->last_data_byte_.load()))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (is_host_command(scan_set)) {
          *replacement = scan_set;
          return HostCommandResult::REPLACE;
        }
        if (scan_set != 0 && scan_set != 0x02) {
          ESP_LOGW(TAG, "Host requested unsupported scan code set 0x%02X", scan_set);
          if (!this->write_response_byte(PS2_RESEND))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (!this->send_ack())
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        if (scan_set == 0 && !this->write_response_byte(0x02))
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        return HostCommandResult::COMPLETE;
      }
    }

    case 0xEE:  // Echo
      ESP_LOGD(TAG, "Host command: echo");
      return this->write_response_byte(0xEE) ? HostCommandResult::COMPLETE : HostCommandResult::COMPLETE_RESPONSE_FAILED;

    case 0xED: {  // Set/reset LEDs
      ESP_LOGD(TAG, "Host command: set LEDs");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      while (true) {
        uint8_t leds = 0;
        if (!this->read_host_argument(&leds))
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        if (leds == PS2_RESEND) {
          if (!this->write_response_byte(this->last_data_byte_.load()))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (is_host_command(leds)) {
          *replacement = leds;
          return HostCommandResult::REPLACE;
        }
        if ((leds & ~0x07U) != 0) {
          if (!this->write_response_byte(PS2_RESEND))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (!this->send_ack())
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        this->set_led_state((leds & LED_CAPS) != 0, (leds & LED_NUM) != 0, (leds & LED_SCROLL) != 0);
        return HostCommandResult::COMPLETE;
      }
    }

    case 0xF7:
    case 0xF8:
    case 0xF9:
      ESP_LOGD(TAG, "Host command: set all-key parameter mode");
      // This emulator does not generate typematic repeats or alter break
      // generation, but these no-argument mode commands are safe to accept.
      return this->send_ack() ? HostCommandResult::COMPLETE : HostCommandResult::COMPLETE_RESPONSE_FAILED;

    case 0xFB:
    case 0xFC:
    case 0xFD: {
      ESP_LOGD(TAG, "Host command: per-key parameter list");
      if (!this->send_ack())
        return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      // These commands are normally meaningful only for scan-code set 3, but
      // hosts still probe them while identifying a keyboard. Consume and
      // acknowledge the list so a Set-2-only emulator does not desynchronize
      // the host; a command byte terminates the list and is handled by the
      // replacement path in handle_host_command().
      while (true) {
        uint8_t key = 0;
        if (!this->read_host_argument(&key))
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
        if (key == PS2_RESEND) {
          if (!this->write_response_byte(this->last_data_byte_.load()))
            return HostCommandResult::COMPLETE_RESPONSE_FAILED;
          continue;
        }
        if (is_host_command(key)) {
          *replacement = key;
          return HostCommandResult::REPLACE;
        }
        if (!is_set3_make_code(key)) {
          ESP_LOGD(TAG, "PS/2 per-key parameter list terminated by 0x%02X", key);
          return HostCommandResult::COMPLETE;
        }
        if (!this->send_ack())
          return HostCommandResult::COMPLETE_RESPONSE_FAILED;
      }
    }

    default:
      ESP_LOGW(TAG, "Host command: unknown 0x%02X", cmd);
      return this->write_response_byte(PS2_RESEND) ? HostCommandResult::COMPLETE
                                                    : HostCommandResult::COMPLETE_RESPONSE_FAILED;
  }
}

bool PS2Keyboard::delay_ms_interruptible(uint32_t delay_ms) {
  uint32_t remaining = delay_ms;
  uint32_t bus_non_idle_since = 0;
  while (remaining != 0) {
    if (this->shutdown_requested_.load())
      return true;
    if (this->host_request_pending() && this->handle_host_request())
      return true;
    if (this->reset_in_progress_.load())
      return true;

    if (!this->bus_idle()) {
      const uint32_t now = millis();
      if (bus_non_idle_since == 0) {
        bus_non_idle_since = now;
      } else if (!this->bus_faulted_.load() &&
                 static_cast<uint32_t>(now - bus_non_idle_since) >= PACKET_SEND_TIMEOUT_MS) {
        this->enter_bus_fault();
        return true;
      }
    } else {
      bus_non_idle_since = 0;
    }

    const uint32_t slice_ms = std::min(remaining, HOST_POLL_SLICE_MS);
    const TickType_t requested_ticks = rtos_delay_ticks(slice_ms);
    const TickType_t max_ticks = rtos_delay_ticks(HOST_POLL_SLICE_MS);
    const TickType_t ticks = std::min(requested_ticks, max_ticks);
    vTaskDelay(ticks);
    // Account for the actual tick duration. At 100 Hz a requested 1 ms
    // slice sleeps for one 10 ms tick; subtracting only the requested value
    // would make a nominal delay run roughly ten times too long.
    const uint32_t elapsed = static_cast<uint32_t>(ticks) * portTICK_PERIOD_MS;
    remaining = elapsed >= remaining ? 0 : remaining - elapsed;
  }
  return false;
}

PS2Keyboard::ByteSendResult PS2Keyboard::send_queued_byte(uint8_t data, bool *frame_started) {
  if (frame_started != nullptr)
    *frame_started = false;
  const uint32_t start = millis();
  bool idle = false;
  uint32_t idle_since = 0;
  uint32_t request_pending_since = 0;
  uint32_t bus_non_idle_since = 0;
  while (true) {
    if (this->shutdown_requested_.load())
      return ByteSendResult::ABORTED;
    if (this->bus_faulted_.load() || !this->reporting_enabled_.load())
      return ByteSendResult::ABORTED;

    const uint32_t now = millis();
    if (this->host_request_pending()) {
      idle = false;
      if (request_pending_since == 0) {
        request_pending_since = now;
      } else if (!this->bus_faulted_.load() &&
                 static_cast<uint32_t>(now - request_pending_since) >= PACKET_SEND_TIMEOUT_MS) {
        this->enter_bus_fault();
        return ByteSendResult::ABORTED;
      }
      // A malformed or aborted host frame can leave RTS asserted. Keep
      // servicing it, but do not let this loop starve process_job's packet
      // watchdog forever.
      if (this->handle_host_request())
        return ByteSendResult::ABORTED;
      continue;
    }
    request_pending_since = 0;

    if (this->bus_idle()) {
      bus_non_idle_since = 0;
      const uint32_t idle_now = micros();
      if (!idle) {
        idle = true;
        idle_since = idle_now;
      }
      if (static_cast<uint32_t>(idle_now - idle_since) >= CLK_IDLE_GUARD_US) {
        idle = false;
        if (this->write_byte(data, frame_started))
          return ByteSendResult::SENT;
        // The host may have inhibited the frame. The caller decides whether
        // to retry this byte alone or replay the whole logical scan chunk.
        return ByteSendResult::INTERRUPTED;
      }
    } else {
      idle = false;
      if (bus_non_idle_since == 0) {
        bus_non_idle_since = now;
      } else if (!this->bus_faulted_.load() &&
                 static_cast<uint32_t>(now - bus_non_idle_since) >= PACKET_SEND_TIMEOUT_MS) {
        this->enter_bus_fault();
        return ByteSendResult::ABORTED;
      }
    }
    if (static_cast<uint32_t>(millis() - start) >= BYTE_SEND_RETRY_INTERVAL_MS)
      return ByteSendResult::TIMED_OUT;
    vTaskDelay(rtos_delay_ticks(1));
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

void PS2Keyboard::reset_device_defaults() {
  this->led_state_.store(0);
  this->led_state_changed_.store(true);
}

void PS2Keyboard::reset_device_state() {
  this->reset_device_defaults();
  this->reporting_enabled_.store(true);
}

void PS2Keyboard::process_job(PS2Job *job) {
  if (job == nullptr)
    return;

  size_t data_offset = 0;
  for (const PS2PacketMeta &packet : job->transaction.packets) {
    if (job->generation != this->output_generation_.load())
      return;
    if (packet.len == 0) {
      if (this->delay_ms_interruptible(packet.delay_after_ms))
        return;
      continue;
    }
    if (data_offset + packet.len > job->transaction.data.size())
      return;
    if (!this->reporting_enabled_.load())
      return;

    const uint32_t packet_start = millis();
    uint8_t i = 0;
    while (i < packet.len) {
      if (job->generation != this->output_generation_.load())
        return;
      if (static_cast<uint32_t>(millis() - packet_start) >= PACKET_SEND_TIMEOUT_MS) {
        this->enter_bus_fault();
        return;
      }
      bool frame_started = false;
      const ByteSendResult result =
          this->send_queued_byte(job->transaction.data[data_offset + i], &frame_started);
      if (result == ByteSendResult::ABORTED)
        return;
      if (result == ByteSendResult::INTERRUPTED) {
        if (packet.retransmit_on_abort && frame_started) {
          // A partially clocked scan-code chunk leaves the host decoder in an
          // intermediate state. Replay the complete logical chunk rather than
          // duplicating only the interrupted byte.
          ESP_LOGW(TAG, "PS/2 chunk interrupted; retransmitting from byte 0");
          i = 0;
        } else {
          // No clock edge was produced, so retrying this byte cannot duplicate
          // a completed frame. This is also the safe behavior for a chunk
          // whose first frame was inhibited before transmission began.
          ESP_LOGW(TAG, "PS/2 frame interrupted; retrying byte %u", static_cast<unsigned>(i));
        }
        continue;
      }
      if (result == ByteSendResult::TIMED_OUT) {
        ESP_LOGW(TAG, "PS/2 output timed out; retrying byte %u", static_cast<unsigned>(i));
        vTaskDelay(rtos_delay_ticks(1));
        continue;
      }
      delayMicroseconds(BYTE_INTERVAL_US);
      i++;
    }
    data_offset += packet.len;
    if (this->delay_ms_interruptible(packet.delay_after_ms))
      return;
  }
}

void PS2Keyboard::task_fn(void *arg) {
  auto *keyboard = reinterpret_cast<PS2Keyboard *>(arg);
  keyboard->run_task();
  if (keyboard->task_exit_semaphore_ != nullptr)
    xSemaphoreGive(keyboard->task_exit_semaphore_);
  vTaskDelete(nullptr);
}

void PS2Keyboard::run_task() {
  // Keep servicing host requests until the power-on BAT is due. A command will
  // invalidate the queued BAT if the host takes control during this interval.
  this->delay_ms_interruptible(this->initial_bat_delay_ms_);

  uint32_t bus_non_idle_since = 0;
  while (!this->shutdown_requested_.load()) {
    if (this->shutdown_requested_.load())
      break;
    if (this->host_request_pending()) {
      const uint32_t now = millis();
      if (bus_non_idle_since == 0)
        bus_non_idle_since = now;
      else if (!this->bus_faulted_.load() &&
               static_cast<uint32_t>(now - bus_non_idle_since) >= PACKET_SEND_TIMEOUT_MS)
        this->enter_bus_fault();
      this->handle_host_request();
      continue;
    }
    if (!this->bus_idle()) {
      const uint32_t now = millis();
      if (bus_non_idle_since == 0)
        bus_non_idle_since = now;
      else if (!this->bus_faulted_.load() &&
               static_cast<uint32_t>(now - bus_non_idle_since) >= PACKET_SEND_TIMEOUT_MS)
        this->enter_bus_fault();
      vTaskDelay(rtos_delay_ticks(1));
      continue;
    }
    bus_non_idle_since = 0;

    void *raw_job = nullptr;
    if (xQueueReceive(this->job_queue_, &raw_job, rtos_delay_ticks(1)) == pdTRUE) {
      auto *job = static_cast<PS2Job *>(raw_job);
      if (job != nullptr && job->generation == this->output_generation_.load()) {
        this->process_job(job);
      }
      this->release_job_memory(job);
    }
  }
}

}  // namespace ps2_keyboard
}  // namespace esphome
