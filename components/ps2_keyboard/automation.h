#pragma once

#include "esphome/core/automation.h"
#include "ps2_keyboard.h"

#include <vector>
#include <string>

namespace esphome {
namespace ps2_keyboard {

template<typename... Ts> class PrintAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  TEMPLATABLE_VALUE(std::string, text)
  TEMPLATABLE_VALUE(uint32_t, delay)

  void play(Ts... x) override {
    auto text = this->text_.value(x...);
    if (this->delay_.has_value()) {
      this->parent_->print(text, this->delay_.value(x...));
    } else {
      this->parent_->print(text);
    }
  }
};

template<typename... Ts> class StrokeAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(uint32_t, delay)

  void play(Ts... x) override {
    auto key = this->key_.value(x...);
    uint32_t delay = this->delay_.has_value() ? this->delay_.value(x...) : 10;
    this->parent_->stroke_key(key, delay);
  }
};

template<typename... Ts> class PressAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  TEMPLATABLE_VALUE(std::string, key)

  void play(Ts... x) override {
    auto key = this->key_.value(x...);
    this->parent_->press_key(key);
  }
};

template<typename... Ts> class ReleaseAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  TEMPLATABLE_VALUE(std::string, key)

  void play(Ts... x) override {
    auto key = this->key_.value(x...);
    this->parent_->release_key(key);
  }
};

template<typename... Ts> class CombinationAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  TEMPLATABLE_VALUE(std::string, keys)
  TEMPLATABLE_VALUE(uint32_t, delay)

  void play(Ts... x) override {
    auto keys = this->keys_.value(x...);
    uint32_t delay = this->delay_.has_value() ? this->delay_.value(x...) : 20;
    this->parent_->press_combination(keys, delay);
  }
};

template<typename... Ts> class SendRawAction : public Action<Ts...>, public Parented<PS2Keyboard> {
 public:
  void set_bytes(const std::vector<uint8_t> &bytes) { this->bytes_ = bytes; }
  void set_bytes(std::function<std::vector<uint8_t>(Ts...)> func) { this->bytes_func_ = func; }

  void play(Ts... x) override {
    if (this->bytes_func_ != nullptr) {
      this->parent_->send_raw_bytes(this->bytes_func_(x...));
    } else {
      this->parent_->send_raw_bytes(this->bytes_);
    }
  }

 protected:
  std::vector<uint8_t> bytes_{};
  std::function<std::vector<uint8_t>(Ts...)> bytes_func_{nullptr};
};

class LEDChangeTrigger : public Trigger<bool, bool, bool> {
 public:
  explicit LEDChangeTrigger(PS2Keyboard *parent) {
    parent->add_on_led_change_callback([this](bool caps, bool num, bool scroll) {
      this->trigger(caps, num, scroll);
    });
  }
};

class HostResetTrigger : public Trigger<> {
 public:
  explicit HostResetTrigger(PS2Keyboard *parent) {
    parent->add_on_host_reset_callback([this]() {
      this->trigger();
    });
  }
};

}  // namespace ps2_keyboard
}  // namespace esphome
