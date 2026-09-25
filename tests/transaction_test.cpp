#include "ps2_keyboard/memory.h"
#include "ps2_keyboard/transactions.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace esphome::ps2_keyboard;

static void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::abort();
  }
}

template<typename ByteContainer>
static void expect_bytes(const ByteContainer &actual, const std::vector<uint8_t> &expected, const char *message) {
  if (actual.size() != expected.size()) {
    expect(false, message);
    return;
  }
  for (size_t i = 0; i < expected.size(); i++)
    expect(actual[i] == expected[i], message);
}

static std::vector<uint8_t> packet_bytes(const PS2Transaction &transaction, size_t packet_index) {
  const PS2PacketMeta &packet = transaction.packets.at(packet_index);
  const size_t begin = packet_index == 0 ? 0 : [&] {
    size_t offset = 0;
    for (size_t i = 0; i < packet_index; i++)
      offset += transaction.packets[i].len;
    return offset;
  }();
  return std::vector<uint8_t>(transaction.data.begin() + begin,
                              transaction.data.begin() + begin + packet.len);
}

static PS2Transaction build_print(const std::string &text, bool caps, uint32_t character_delay_ms,
                                  PS2PrintPlan *expected_plan = nullptr, uint32_t *expected_unsupported = nullptr) {
  PS2PrintPlan plan;
  uint32_t unsupported = 0;
  expect(plan_print_transaction(text, caps, plan, unsupported), "print transaction should fit its limits");
  if (expected_plan != nullptr)
    *expected_plan = plan;
  if (expected_unsupported != nullptr)
    *expected_unsupported = unsupported;

  PS2Transaction transaction;
  expect(build_print_transaction(text, caps, character_delay_ms, plan, transaction, unsupported),
         "print transaction should build");
  expect(unsupported == 0 || expected_unsupported != nullptr, "unexpected unsupported print character");
  expect(transaction.data.capacity() == plan.data_bytes, "print data reservation should be exact");
  expect(transaction.packets.capacity() == plan.packet_count, "print packet reservation should be exact");
  return transaction;
}

int main() {
  // Reservation accounting must replace the estimate in one atomic step. In
  // particular, an actual allocation larger than its estimate must not leave
  // the budget reduced by both values.
  std::atomic<size_t> committed{0};
  expect(memory_detail::reserve(committed, 1024, 100), "memory reservation should fit");
  expect(committed.load() == 100, "reservation should be committed");
  expect(memory_detail::replace_reservation(committed, 1024, 100, 150),
         "over-capacity actual allocation within the budget should commit");
  expect(committed.load() == 150, "replacing a reservation should retain the actual allocation");
  expect(memory_detail::replace_reservation(committed, 1024, 150, 80),
         "under-capacity actual allocation should commit");
  expect(committed.load() == 80, "replacing a reservation should release the unused estimate");
  expect(!memory_detail::replace_reservation(committed, 1024, 80, 1025),
         "actual allocation over the budget should fail");
  expect(committed.load() == 80, "failed replacement must leave the reservation intact");
  expect(memory_detail::reserve(committed, 1024, 900), "second reservation should fit");
  expect(!memory_detail::replace_reservation(committed, 1024, 900, 1000),
         "replacement must account for other outstanding reservations");
  expect(committed.load() == 980, "failed replacement must not disturb another reservation");
  expect(memory_detail::release(committed, 900), "second exact memory release should succeed");
  expect(memory_detail::release(committed, 80), "exact memory release should succeed");
  expect(committed.load() == 0, "memory release should empty the budget");
  expect(!memory_detail::release(committed, 1), "release below zero must fail");
  expect(!memory_detail::replace_reservation(committed, 1024, 1, 1),
         "replacement without a reservation must fail");

  const memory_detail::HeapSnapshot healthy_heap{100000, 50000};
  expect(memory_detail::transaction_fits(healthy_heap, 64, 4096, 2048, 16 * 1024),
         "healthy current heap should admit a transaction");
  const memory_detail::HeapSnapshot fragmented_heap{100000, 128};
  expect(!memory_detail::transaction_fits(fragmented_heap, 64, 4096, 2048, 16 * 1024),
         "fragmented heap should reject a transaction when its largest block is too small");
  const memory_detail::HeapSnapshot small_heap{128, 128};
  expect(!memory_detail::transaction_fits(small_heap, 64, 1, 1, 16 * 1024),
         "insufficient total free heap should reject a transaction");
  expect(!memory_detail::transaction_fits({SIZE_MAX, SIZE_MAX}, SIZE_MAX, 1, 1, 1),
         "overflowing heap arithmetic should reject a transaction");
  PS2DataBuffer oversized_data;
  expect(!oversized_data.reserve(MAX_JOB_DATA_BYTES + 1) && oversized_data.allocation_failed(),
         "byte buffer should reject an over-limit allocation without throwing");
  PS2PacketBuffer oversized_packets;
  expect(!oversized_packets.reserve(MAX_JOB_PACKETS + 1) && oversized_packets.allocation_failed(),
         "packet buffer should reject an over-limit allocation without throwing");

  PS2PrintPlan plan;
  uint32_t unsupported = 0;
  expect(plan_print_transaction("a", false, plan, unsupported), "lowercase print should fit");
  expect(plan.data_bytes == 3 && plan.packet_count == 2, "lowercase print should use make/break packets");

  PS2Transaction lower = build_print("a", false, 7);
  expect_bytes(lower.data, {0x1C, 0xF0, 0x1C}, "lowercase scan order");
  expect(lower.packets.size() == 2, "lowercase packet count");
  expect(lower.packets[0].len == 1 && lower.packets[0].delay_after_ms == 0, "lowercase make delay");
  expect(lower.packets[1].len == 2 && lower.packets[1].delay_after_ms == 7, "lowercase break delay");
  expect(lower.packets[0].retransmit_on_abort && lower.packets[1].retransmit_on_abort,
         "scan packets should be replayable as logical units");
  PS2Transaction moved_lower(std::move(lower));
  expect(moved_lower.data.size() == 3 && moved_lower.packets.size() == 2,
         "moving a transaction should preserve its compact buffers");
  expect(lower.data.empty() && lower.packets.empty(), "moved-from transaction buffers should be empty");

  PS2Transaction upper = build_print("A", false, 7);
  expect_bytes(upper.data, {0x12, 0x1C, 0xF0, 0x1C, 0xF0, 0x12}, "uppercase scan order");
  expect(upper.packets.size() == 4, "uppercase packet count");
  expect(upper.packets[0].delay_after_ms == 0 && upper.packets[1].delay_after_ms == 0 &&
             upper.packets[2].delay_after_ms == 0 && upper.packets[3].delay_after_ms == 7,
         "uppercase delay belongs after shift release");

  PS2Transaction punctuation = build_print("!", false, 9);
  expect_bytes(punctuation.data, {0x12, 0x16, 0xF0, 0x16, 0xF0, 0x12}, "shifted punctuation scan order");

  PS2Transaction caps_lower = build_print("a", true, 5);
  expect_bytes(caps_lower.data, {0x12, 0x1C, 0xF0, 0x1C, 0xF0, 0x12}, "Caps Lock flips lowercase shift");
  PS2Transaction caps_upper = build_print("A", true, 5);
  expect_bytes(caps_upper.data, {0x1C, 0xF0, 0x1C}, "Caps Lock flips uppercase shift");

  PS2Transaction mixed = build_print("aA!", false, 11);
  expect_bytes(mixed.data,
               {0x1C, 0xF0, 0x1C, 0x12, 0x1C, 0xF0, 0x1C, 0xF0, 0x12, 0x12, 0x16, 0xF0, 0x16, 0xF0, 0x12},
               "mixed print scan order");
  expect(mixed.packets.size() == 10, "mixed print packet count");

  uint32_t expected_unsupported = 0;
  PS2Transaction skipped = build_print(std::string("a\x01", 2), false, 5, nullptr, &expected_unsupported);
  expect(expected_unsupported == 1, "unsupported print character should be counted");
  expect_bytes(skipped.data, {0x1C, 0xF0, 0x1C}, "unsupported print character should be skipped");

  PS2Transaction lower_max = build_print(std::string(MAX_PRINT_BYTES, 'a'), false, 5);
  expect(lower_max.data.size() == MAX_PRINT_BYTES * 3, "1024 lowercase characters use three scan bytes each");
  expect(lower_max.packets.size() == MAX_PRINT_BYTES * 2, "1024 lowercase print packet count");
  PS2Transaction upper_max = build_print(std::string(MAX_PRINT_BYTES, 'A'), false, 5);
  expect(upper_max.data.size() == MAX_PRINT_BYTES * 6, "1024 uppercase character data size");
  expect(upper_max.packets.size() == MAX_PRINT_BYTES * 4, "1024 uppercase print packet count");
  expect(!plan_print_transaction(std::string(MAX_PRINT_BYTES + 1, 'a'), false, plan, unsupported),
         "print input over 1024 bytes should be rejected");

  std::vector<uint8_t> raw(33);
  for (size_t i = 0; i < raw.size(); i++)
    raw[i] = static_cast<uint8_t>(i);
  PS2Transaction raw_transaction;
  expect(build_raw_transaction(raw, raw_transaction), "raw transaction should build");
  expect(raw_transaction.data == raw, "raw transaction should preserve byte order");
  expect(raw_transaction.packets.size() == 3, "raw transaction should use fixed 16-byte chunks");
  expect(raw_transaction.packets[0].len == 16 && raw_transaction.packets[1].len == 16 &&
             raw_transaction.packets[2].len == 1,
         "raw transaction chunk sizes");
  for (const PS2PacketMeta &packet : raw_transaction.packets)
    expect(!packet.retransmit_on_abort, "raw packets must not replay already completed bytes");
  expect_bytes(packet_bytes(raw_transaction, 0), std::vector<uint8_t>(raw.begin(), raw.begin() + 16),
               "first raw packet bytes");
  std::vector<uint8_t> maximum_raw(MAX_RAW_BYTES);
  for (size_t i = 0; i < maximum_raw.size(); i++)
    maximum_raw[i] = static_cast<uint8_t>(i);
  expect(build_raw_transaction(maximum_raw, raw_transaction), "4096-byte raw transaction should build");
  expect(raw_transaction.data.size() == MAX_RAW_BYTES && raw_transaction.packets.size() == MAX_RAW_BYTES / 16,
         "4096-byte raw transaction should use 256 fixed chunks");
  expect(!build_raw_transaction(std::vector<uint8_t>(MAX_RAW_BYTES + 1), raw_transaction),
         "raw transaction over 4096 bytes should be rejected");

  std::cout << "transaction_test: all checks passed\n";
  return 0;
}
