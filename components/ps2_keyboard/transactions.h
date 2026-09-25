#pragma once

#include "scancodes.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace ps2_keyboard {

static constexpr uint8_t PS2_PACKET_DATA_BYTES = 16;
static constexpr size_t MAX_JOB_PACKETS = 4096;
static constexpr size_t MAX_JOB_DATA_BYTES = 16384;
static constexpr size_t MAX_PRINT_BYTES = 1024;
static constexpr size_t MAX_RAW_BYTES = 4096;
static constexpr size_t MAX_COMBINATION_KEYS = 32;

struct PS2Packet {
  uint8_t len{0};
  uint8_t data[PS2_PACKET_DATA_BYTES]{0};
  uint32_t delay_after_ms{0};
  bool retransmit_on_abort{false};
};

struct PS2PacketMeta {
  uint8_t len{0};
  uint32_t delay_after_ms{0};
  bool retransmit_on_abort{false};
};

// std::vector::reserve() aborts when the firmware is built with exceptions
// disabled. These small, vector-like buffers use nothrow array allocations so
// a failed output transaction can be rejected without taking down the device.
class PS2DataBuffer {
 public:
  PS2DataBuffer() = default;
  PS2DataBuffer(const PS2DataBuffer &) = delete;
  PS2DataBuffer &operator=(const PS2DataBuffer &) = delete;
  PS2DataBuffer(PS2DataBuffer &&other) noexcept
      : data_(std::move(other.data_)), size_(other.size_), capacity_(other.capacity_),
        allocation_failed_(other.allocation_failed_) {
    other.size_ = 0;
    other.capacity_ = 0;
    other.allocation_failed_ = false;
  }
  PS2DataBuffer &operator=(PS2DataBuffer &&other) noexcept {
    if (this != &other) {
      data_ = std::move(other.data_);
      size_ = other.size_;
      capacity_ = other.capacity_;
      allocation_failed_ = other.allocation_failed_;
      other.size_ = 0;
      other.capacity_ = 0;
      other.allocation_failed_ = false;
    }
    return *this;
  }

  bool reserve(size_t count) {
    if (count > MAX_JOB_DATA_BYTES) {
      allocation_failed_ = true;
      return false;
    }
    if (count <= capacity_)
      return true;
    std::unique_ptr<uint8_t[]> next(new (std::nothrow) uint8_t[count]);
    if (next == nullptr) {
      allocation_failed_ = true;
      return false;
    }
    if (size_ != 0)
      std::copy(data_.get(), data_.get() + size_, next.get());
    data_ = std::move(next);
    capacity_ = count;
    return true;
  }

  bool insert(const uint8_t *first, const uint8_t *last) {
    if (first == last)
      return true;
    if (first == nullptr || last == nullptr) {
      allocation_failed_ = true;
      return false;
    }
    const size_t count = static_cast<size_t>(last - first);
    if (size_ > MAX_JOB_DATA_BYTES || count > MAX_JOB_DATA_BYTES - size_) {
      allocation_failed_ = true;
      return false;
    }
    const size_t required = size_ + count;
    if (required > capacity_ && !reserve(required))
      return false;
    if (count != 0)
      std::copy(first, last, data_.get() + size_);
    size_ = required;
    return true;
  }

  bool push_back(uint8_t value) {
    if (size_ >= MAX_JOB_DATA_BYTES) {
      allocation_failed_ = true;
      return false;
    }
    if (size_ == capacity_ && !reserve(size_ + 1))
      return false;
    data_[size_++] = value;
    return true;
  }

  void clear() {
    size_ = 0;
    allocation_failed_ = false;
  }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool allocation_failed() const { return allocation_failed_; }
  uint8_t *data() { return data_.get(); }
  const uint8_t *data() const { return data_.get(); }
  const uint8_t &operator[](size_t index) const {
    if (index >= size_)
      std::abort();
    return data_[index];
  }
  uint8_t *begin() { return data_.get(); }
  const uint8_t *begin() const { return data_.get(); }
  uint8_t *end() { return data_.get() + size_; }
  const uint8_t *end() const { return data_.get() + size_; }

 private:
  std::unique_ptr<uint8_t[]> data_{};
  size_t size_{0};
  size_t capacity_{0};
  bool allocation_failed_{false};
};

class PS2PacketBuffer {
 public:
  PS2PacketBuffer() = default;
  PS2PacketBuffer(const PS2PacketBuffer &) = delete;
  PS2PacketBuffer &operator=(const PS2PacketBuffer &) = delete;
  PS2PacketBuffer(PS2PacketBuffer &&other) noexcept
      : packets_(std::move(other.packets_)), size_(other.size_), capacity_(other.capacity_),
        allocation_failed_(other.allocation_failed_) {
    other.size_ = 0;
    other.capacity_ = 0;
    other.allocation_failed_ = false;
  }
  PS2PacketBuffer &operator=(PS2PacketBuffer &&other) noexcept {
    if (this != &other) {
      packets_ = std::move(other.packets_);
      size_ = other.size_;
      capacity_ = other.capacity_;
      allocation_failed_ = other.allocation_failed_;
      other.size_ = 0;
      other.capacity_ = 0;
      other.allocation_failed_ = false;
    }
    return *this;
  }

  bool reserve(size_t count) {
    if (count > MAX_JOB_PACKETS) {
      allocation_failed_ = true;
      return false;
    }
    if (count <= capacity_)
      return true;
    std::unique_ptr<PS2PacketMeta[]> next(new (std::nothrow) PS2PacketMeta[count]);
    if (next == nullptr) {
      allocation_failed_ = true;
      return false;
    }
    if (size_ != 0)
      std::copy(packets_.get(), packets_.get() + size_, next.get());
    packets_ = std::move(next);
    capacity_ = count;
    return true;
  }

  bool push_back(const PS2PacketMeta &packet) {
    if (size_ >= MAX_JOB_PACKETS) {
      allocation_failed_ = true;
      return false;
    }
    if (size_ == capacity_ && !reserve(size_ + 1))
      return false;
    packets_[size_++] = packet;
    return true;
  }

  PS2PacketMeta &at(size_t index) {
    if (index >= size_)
      std::abort();
    return packets_[index];
  }
  const PS2PacketMeta &at(size_t index) const {
    if (index >= size_)
      std::abort();
    return packets_[index];
  }

  PS2PacketMeta &operator[](size_t index) { return this->at(index); }
  const PS2PacketMeta &operator[](size_t index) const { return this->at(index); }

  void clear() {
    size_ = 0;
    allocation_failed_ = false;
  }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool allocation_failed() const { return allocation_failed_; }
  PS2PacketMeta *data() { return packets_.get(); }
  const PS2PacketMeta *data() const { return packets_.get(); }
  PS2PacketMeta *begin() { return packets_.get(); }
  const PS2PacketMeta *begin() const { return packets_.get(); }
  PS2PacketMeta *end() { return packets_.get() + size_; }
  const PS2PacketMeta *end() const { return packets_.get() + size_; }

 private:
  std::unique_ptr<PS2PacketMeta[]> packets_{};
  size_t size_{0};
  size_t capacity_{0};
  bool allocation_failed_{false};
};

template<typename Allocator>
bool operator==(const PS2DataBuffer &buffer, const std::vector<uint8_t, Allocator> &values) {
  if (buffer.size() != values.size())
    return false;
  return buffer.size() == 0 || std::equal(buffer.begin(), buffer.end(), values.begin());
}

template<typename Allocator>
bool operator==(const std::vector<uint8_t, Allocator> &values, const PS2DataBuffer &buffer) {
  return buffer == values;
}

struct PS2Transaction {
  PS2DataBuffer data{};
  PS2PacketBuffer packets{};
};

inline bool append_packet(PS2Transaction &transaction, const PS2Packet &packet) {
  if (transaction.data.allocation_failed() || transaction.packets.allocation_failed() ||
      transaction.packets.size() >= MAX_JOB_PACKETS || packet.len > sizeof(packet.data) ||
      transaction.data.size() > MAX_JOB_DATA_BYTES ||
      packet.len > MAX_JOB_DATA_BYTES - transaction.data.size())
    return false;
  if (packet.len != 0 && !transaction.data.insert(packet.data, packet.data + packet.len))
    return false;
  if (!transaction.packets.push_back(PS2PacketMeta{packet.len, packet.delay_after_ms, packet.retransmit_on_abort}))
    return false;
  return true;
}

inline bool append_scan_code_to_packet(PS2Packet &packet, Key key, bool release) {
  uint8_t sequence[PS2_PACKET_DATA_BYTES]{};
  uint8_t new_length = 0;
  const bool ok = release ? get_break_code(key, sequence, new_length) : get_make_code(key, sequence, new_length);
  if (!ok || new_length > sizeof(packet.data) - packet.len)
    return false;
  for (uint8_t i = 0; i < new_length; i++)
    packet.data[packet.len + i] = sequence[i];
  packet.len = static_cast<uint8_t>(packet.len + new_length);
  return true;
}

// A packet made from one scan-code sequence is replayed as a unit if a clock
// edge interrupts it. Raw packets intentionally do not use this behavior:
// there is no protocol-level boundary that would make replaying completed
// prefix bytes safe.
inline bool append_scan_code(PS2Transaction &transaction, Key key, bool release, uint32_t delay_after_ms) {
  PS2Packet packet{};
  if (!append_scan_code_to_packet(packet, key, release))
    return false;
  packet.delay_after_ms = delay_after_ms;
  packet.retransmit_on_abort = true;
  return append_packet(transaction, packet);
}

inline bool append_delay(PS2Transaction &transaction, uint32_t delay_after_ms) {
  if (delay_after_ms == 0)
    return true;
  PS2Packet packet{};
  packet.delay_after_ms = delay_after_ms;
  return append_packet(transaction, packet);
}

inline bool is_letter_key(Key key) {
  return key >= Key::KEY_A && key <= Key::KEY_Z;
}

inline bool scan_code_lengths(Key key, size_t &make_length, size_t &break_length) {
  uint8_t sequence[PS2_PACKET_DATA_BYTES]{};
  uint8_t length = 0;
  if (!get_make_code(key, sequence, length)) {
    make_length = 0;
    break_length = 0;
    return false;
  }
  make_length = length;
  if (!get_break_code(key, sequence, length)) {
    make_length = 0;
    break_length = 0;
    return false;
  }
  break_length = length;
  return true;
}

struct PS2PrintPlan {
  size_t data_bytes{0};
  size_t packet_count{0};
};

inline bool print_character_parameters(char c, bool caps_lock_enabled, Key &key, bool &shift) {
  if (!ascii_to_key(c, key, shift))
    return false;
  if (is_letter_key(key) && caps_lock_enabled)
    shift = !shift;
  return true;
}

inline bool plan_print_transaction(const std::string &text, bool caps_lock_enabled, PS2PrintPlan &plan,
                                   uint32_t &unsupported) {
  plan = PS2PrintPlan{};
  unsupported = 0;
  if (text.size() > MAX_PRINT_BYTES)
    return false;

  for (char c : text) {
    Key key = Key::KEY_NONE;
    bool shift = false;
    if (!print_character_parameters(c, caps_lock_enabled, key, shift)) {
      unsupported++;
      continue;
    }

    size_t make_length = 0;
    size_t break_length = 0;
    if (!scan_code_lengths(key, make_length, break_length)) {
      unsupported++;
      continue;
    }
    size_t character_data_bytes = make_length + break_length;
    size_t character_packet_count = 2;
    if (shift) {
      size_t shift_make_length = 0;
      size_t shift_break_length = 0;
      if (!scan_code_lengths(Key::KEY_LSHIFT, shift_make_length, shift_break_length)) {
        unsupported++;
        continue;
      }
      character_data_bytes += shift_make_length + shift_break_length;
      character_packet_count = 4;
    }
    if (plan.packet_count > MAX_JOB_PACKETS - character_packet_count ||
        plan.data_bytes > MAX_JOB_DATA_BYTES - character_data_bytes)
      return false;
    plan.packet_count += character_packet_count;
    plan.data_bytes += character_data_bytes;
  }
  return true;
}

inline bool build_print_transaction(const std::string &text, bool caps_lock_enabled,
                                   uint32_t character_delay_ms, const PS2PrintPlan &plan,
                                   PS2Transaction &transaction, uint32_t &unsupported) {
  transaction.data.clear();
  transaction.packets.clear();
  unsupported = 0;

  if (!transaction.data.reserve(plan.data_bytes) || !transaction.packets.reserve(plan.packet_count)) {
    transaction.data.clear();
    transaction.packets.clear();
    return false;
  }

  for (char c : text) {
    Key key = Key::KEY_NONE;
    bool shift = false;
    if (!print_character_parameters(c, caps_lock_enabled, key, shift)) {
      unsupported++;
      continue;
    }
    bool valid = true;
    if (shift)
      valid = append_scan_code(transaction, Key::KEY_LSHIFT, false, 0);
    if (valid)
      valid = append_scan_code(transaction, key, false, 0);
    if (valid)
      valid = append_scan_code(transaction, key, true, shift ? 0 : character_delay_ms);
    if (valid && shift)
      valid = append_scan_code(transaction, Key::KEY_LSHIFT, true, character_delay_ms);
    if (!valid) {
      transaction.data.clear();
      transaction.packets.clear();
      return false;
    }
  }
  if (transaction.data.size() != plan.data_bytes || transaction.packets.size() != plan.packet_count ||
      transaction.data.allocation_failed() || transaction.packets.allocation_failed()) {
    transaction.data.clear();
    transaction.packets.clear();
    return false;
  }
  return true;
}

inline bool build_raw_transaction(const std::vector<uint8_t> &bytes, PS2Transaction &transaction) {
  transaction.data.clear();
  transaction.packets.clear();
  if (bytes.empty() || bytes.size() > MAX_RAW_BYTES)
    return false;
  const size_t packet_count = (bytes.size() + PS2_PACKET_DATA_BYTES - 1) / PS2_PACKET_DATA_BYTES;
  if (!transaction.packets.reserve(packet_count) || !transaction.data.reserve(bytes.size())) {
    transaction.data.clear();
    transaction.packets.clear();
    return false;
  }
  for (size_t offset = 0; offset < bytes.size();) {
    PS2Packet packet{};
    const size_t remaining = bytes.size() - offset;
    const size_t chunk = remaining < PS2_PACKET_DATA_BYTES ? remaining : PS2_PACKET_DATA_BYTES;
    packet.len = static_cast<uint8_t>(chunk);
    for (size_t i = 0; i < chunk; i++)
      packet.data[i] = bytes[offset + i];
    packet.delay_after_ms = 5;
    // Raw bytes have no scan-sequence boundary. Retrying only the incomplete
    // frame avoids duplicating bytes that the host already received.
    packet.retransmit_on_abort = false;
    if (!append_packet(transaction, packet)) {
      transaction.data.clear();
      transaction.packets.clear();
      return false;
    }
    offset += chunk;
  }
  return !transaction.data.allocation_failed() && !transaction.packets.allocation_failed();
}

}  // namespace ps2_keyboard
}  // namespace esphome
