#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#include "scancodes.h"

#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
#include "esphome/components/api/custom_api_device.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace ps2_keyboard {

struct PS2Transaction;
struct PS2Job;

class PS2Keyboard : public Component
#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
    , public api::CustomAPIDevice
#endif
{
 public:
  PS2Keyboard();

  void set_clk_pin(InternalGPIOPin *clk_pin) { this->clk_pin_ = clk_pin; }
  void set_data_pin(InternalGPIOPin *data_pin) { this->data_pin_ = data_pin; }
  void set_task_priority(uint8_t priority) { this->task_priority_ = priority; }
  void set_task_core(int8_t core) { this->task_core_ = core; }
  void set_service_prefix(std::string prefix) { this->service_prefix_ = std::move(prefix); }

#ifdef USE_BINARY_SENSOR
  void set_caps_lock_sensor(binary_sensor::BinarySensor *sensor) { this->caps_lock_sensor_ = sensor; }
  void set_num_lock_sensor(binary_sensor::BinarySensor *sensor) { this->num_lock_sensor_ = sensor; }
  void set_scroll_lock_sensor(binary_sensor::BinarySensor *sensor) { this->scroll_lock_sensor_ = sensor; }
#endif

  void add_on_led_change_callback(std::function<void(bool, bool, bool)> &&callback) {
    this->led_change_callback_.add(std::move(callback));
  }
  void add_on_host_reset_callback(std::function<void()> &&callback) {
    this->host_reset_callback_.add(std::move(callback));
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  bool teardown() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // High-level public keyboard actions
  void press_key(Key key);
  void press_key(const std::string &key_name);
  void release_key(Key key);
  void release_key(const std::string &key_name);
  void stroke_key(Key key, uint32_t delay_ms = 10);
  void stroke_key(const std::string &key_name, uint32_t delay_ms = 10);
  void print(const std::string &text, uint32_t inter_key_delay_ms = 10);
  void press_combination(const std::vector<Key> &keys, uint32_t hold_delay_ms = 20);
  void press_combination(const std::string &combo_str, uint32_t hold_delay_ms = 20);
  void send_raw_bytes(const std::vector<uint8_t> &bytes);

#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
  // Optional Home Assistant custom API services
  void ha_type(std::string text);
  void ha_stroke(std::string key);
  void ha_press(std::string key);
  void ha_release(std::string key);
  void ha_combination(std::string keys);
  void ha_send_raw(std::string hex_bytes);
#endif

 protected:
  // Bus driver methods and task entry points are intentionally not part of
  // the public component API; the dedicated task serializes all bus access.
  bool write_byte(uint8_t data, bool *frame_started = nullptr);
  bool write_byte_wait_idle(uint8_t data, uint32_t timeout_us = 15000, bool *frame_started = nullptr);
  bool read_byte(uint8_t *result, uint32_t timeout_us = 50000, bool *aborted = nullptr,
                 bool *invalid = nullptr);
  bool read_host_argument(uint8_t *result);
  bool send_ack();
  bool handle_host_command(uint8_t cmd);

  static void task_fn(void *arg);
  void run_task();

  InternalGPIOPin *clk_pin_{nullptr};
  InternalGPIOPin *data_pin_{nullptr};
  ISRInternalGPIOPin isr_clk_{};
  ISRInternalGPIOPin isr_data_{};

  enum class ByteSendResult : uint8_t { SENT, INTERRUPTED, ABORTED, TIMED_OUT };
  enum class HostCommandResult : uint8_t { COMPLETE, COMPLETE_RESPONSE_FAILED, REPLACE };

  bool bus_idle();
  bool host_request_pending();
  void press_key_with_shift(Key key, bool shift);
  void release_key_with_shift(Key key, bool shift);
  void stroke_key_with_shift(Key key, bool shift, uint32_t delay_ms);
  void press_combination(const std::vector<KeyCombinationPart> &keys, uint32_t hold_delay_ms);
  size_t reserve_output_memory(size_t data_bytes, size_t packet_count);
  void release_output_memory(size_t reservation);
  bool enqueue_job(PS2Transaction &&transaction, size_t reservation);
  void enter_bus_fault();
  void drop_pending_jobs_locked();
  void cleanup_runtime_resources();
  void invalidate_output_jobs();
  bool handle_host_request();
  HostCommandResult handle_host_command_impl(uint8_t cmd, uint8_t *replacement);
  bool delay_ms_interruptible(uint32_t delay_ms);
  bool write_response_byte(uint8_t data);
  bool write_response_bytes(const uint8_t *data, size_t length);
  bool send_resend_request();
  ByteSendResult send_queued_byte(uint8_t data, bool *frame_started = nullptr);
  void process_job(PS2Job *job);
  // Destroys the job and releases its exact allocation charge.
  void release_job_memory(PS2Job *job);
  void set_led_state(bool caps, bool num, bool scroll);
  void reset_device_defaults();
  void reset_device_state();

  QueueHandle_t job_queue_{nullptr};
  SemaphoreHandle_t queue_mutex_{nullptr};
  SemaphoreHandle_t task_exit_semaphore_{nullptr};
  TaskHandle_t task_handle_{nullptr};
  std::atomic<bool> shutdown_requested_{false};
  uint8_t task_priority_{10};
  int8_t task_core_{-1};
  uint32_t initial_bat_delay_ms_{0};
  std::string service_prefix_{"ps2"};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *caps_lock_sensor_{nullptr};
  binary_sensor::BinarySensor *num_lock_sensor_{nullptr};
  binary_sensor::BinarySensor *scroll_lock_sensor_{nullptr};
#endif

  CallbackManager<void(bool, bool, bool)> led_change_callback_{};
  CallbackManager<void()> host_reset_callback_{};

  bool led_state_initialized_{false};
  std::atomic<uint8_t> led_state_{0};
  std::atomic<bool> led_state_changed_{false};
  std::atomic<bool> host_reset_detected_{false};
  std::atomic<bool> reporting_enabled_{true};
  std::atomic<bool> reset_in_progress_{false};
  std::atomic<bool> host_command_in_progress_{false};
  std::atomic<uint32_t> output_generation_{0};
  std::atomic<bool> bus_faulted_{false};
  std::atomic<uint8_t> last_data_byte_{0xAA};
};

}  // namespace ps2_keyboard
}  // namespace esphome
