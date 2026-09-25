#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>

namespace esphome {
namespace ps2_keyboard {
namespace memory_detail {

inline bool checked_add(size_t left, size_t right, size_t &result) {
  if (right > std::numeric_limits<size_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

inline bool checked_multiply(size_t left, size_t right, size_t &result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

// Reserve an amount without allowing the committed value to exceed limit.
inline bool reserve(std::atomic<size_t> &committed, size_t limit, size_t amount) {
  if (amount == 0 || amount > limit)
    return false;

  size_t current = committed.load();
  do {
    if (current > limit || amount > limit - current)
      return false;
  } while (!committed.compare_exchange_weak(current, current + amount));
  return true;
}

// Replace the amount held by one outstanding reservation with its actual
// allocation. The old reservation is removed and the actual value is added in
// one CAS, so concurrent producers cannot observe or modify an intermediate
// accounting state.
inline bool replace_reservation(std::atomic<size_t> &committed, size_t limit, size_t reserved, size_t actual) {
  if (reserved == 0 || actual == 0 || reserved > limit)
    return false;

  size_t current = committed.load();
  while (true) {
    // A reservation must still be present in the budget. Treat a violated
    // invariant as a failed commit rather than underflowing the counter.
    if (current > limit || current < reserved)
      return false;
    const size_t without_reservation = current - reserved;
    if (actual > limit - without_reservation)
      return false;
    const size_t next = without_reservation + actual;
    if (committed.compare_exchange_weak(current, next))
      return true;
  }
}

inline bool release(std::atomic<size_t> &committed, size_t amount) {
  if (amount == 0)
    return true;

  size_t current = committed.load();
  do {
    if (current < amount)
      return false;
    const size_t next = current - amount;
    if (committed.compare_exchange_weak(current, next))
      return true;
  } while (true);
}

struct HeapSnapshot {
  size_t free_bytes{0};
  size_t largest_block_bytes{0};
};

// A transaction has three separately allocated objects: its byte buffer,
// packet-metadata buffer, and PS2Job. The total free-byte check protects the
// overall heap budget; the largest-block check prevents accepting a large
// request when fragmentation means one of those allocations cannot succeed.
inline bool transaction_fits(const HeapSnapshot &heap, size_t job_bytes, size_t data_bytes, size_t packet_bytes,
                             size_t headroom) {
  constexpr size_t ALLOCATION_SLACK = 256;
  size_t required = 0;
  size_t term = 0;
  if (!checked_add(job_bytes, data_bytes, term) || !checked_add(term, packet_bytes, term) ||
      !checked_add(term, headroom, required))
    return false;

  size_t job_allocation = 0;
  size_t data_allocation = 0;
  size_t packet_allocation = 0;
  if (!checked_add(job_bytes, ALLOCATION_SLACK, job_allocation) ||
      !checked_add(data_bytes, ALLOCATION_SLACK, data_allocation) ||
      !checked_add(packet_bytes, ALLOCATION_SLACK, packet_allocation))
    return false;

  const size_t largest_required = std::max({job_allocation, data_allocation, packet_allocation});
  return heap.free_bytes >= required && heap.largest_block_bytes >= largest_required;
}

}  // namespace memory_detail
}  // namespace ps2_keyboard
}  // namespace esphome
