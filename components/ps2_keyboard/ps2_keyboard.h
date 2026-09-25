#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#include "scancodes.h"

#ifdef USE_API
#include "esphome/components/api/custom_api_device.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <vector>
#include <string>

namespace esphome {
namespace ps2_keyboard {

struct PS2Packet {
  uint8_t len{0};
  uint8_t data[16]{0};
  uint16_t delay_after_ms{10};
};

class PS2Keyboard : public Component
#ifdef USE_API
    , public api::CustomAPIDevice
#endif
{
 public:
  PS2Keyboard();

  void set_clk_pin(InternalGPIOPin *clk_pin) { this->clk_pin_ = clk_pin; }
  void set_data_pin(InternalGPIOPin *data_pin) { this->data_pin_ = data_pin; }
  void set_task_priority(uint8_t priority) { this->task_priority_ = priority; }
  void set_task_core(int8_t core) { this->task_core_ = core; }

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

#ifdef USE_API
  // Home Assistant User Services
  void ha_type(std::string text);
  void ha_stroke(std::string key);
  void ha_press(std::string key);
  void ha_release(std::string key);
  void ha_combination(std::string keys);
  void ha_send_raw(std::string hex_bytes);
#endif

  // Bus driver methods
  bool write_byte(uint8_t data);
  bool write_byte_wait_idle(uint8_t data, uint32_t timeout_us = 15000);
  bool read_byte(uint8_t *result, uint32_t timeout_us = 50000);
  void send_ack();
  void handle_host_command(uint8_t cmd);

  // Background task entry point
  static void task_fn(void *arg);
  void run_task();

 protected:
  InternalGPIOPin *clk_pin_{nullptr};
  InternalGPIOPin *data_pin_{nullptr};
  ISRInternalGPIOPin isr_clk_{};
  ISRInternalGPIOPin isr_data_{};

  QueueHandle_t send_queue_{nullptr};
  TaskHandle_t task_handle_{nullptr};
  uint8_t task_priority_{10};
  int8_t task_core_{1};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *caps_lock_sensor_{nullptr};
  binary_sensor::BinarySensor *num_lock_sensor_{nullptr};
  binary_sensor::BinarySensor *scroll_lock_sensor_{nullptr};
#endif

  CallbackManager<void(bool, bool, bool)> led_change_callback_{};
  CallbackManager<void()> host_reset_callback_{};

  volatile bool caps_lock_state_{false};
  volatile bool num_lock_state_{false};
  volatile bool scroll_lock_state_{false};
  volatile bool led_state_changed_{false};
  volatile bool host_reset_detected_{false};
  volatile bool reporting_enabled_{true};
  volatile uint8_t last_sent_byte_{0xAA};
};

}  // namespace ps2_keyboard
}  // namespace esphome
