#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cymon {

/// A single timestamped sample.
struct Sample {
  uint64_t timestamp_us{0};  ///< microseconds — esp_timer_get_time() on target
  float value{0.0F};
};

/// Fixed-capacity lock-free ring buffer for timestamped float samples.
///
/// Template parameters:
///   Capacity — maximum number of samples stored (must be a power of two for
///              efficient modular indexing).
///
/// Thread-safety model:
///   Single-producer / single-consumer.  The write head is an atomic<size_t>;
///   the reader reads it once with acquire and walks backwards without a lock.
///   On FreeRTOS / ESP32 this is ISR-safe for the producer.
///
/// Rolling mode (always enabled): when the buffer is full the oldest entry is
/// silently overwritten.  There is NO interpolation — samples are stored and
/// returned as-is.
template <size_t Capacity>
class TimestampedRingBuffer {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  static constexpr size_t kCapacity = Capacity;

  /// Reset the buffer to empty.
  void Clear() noexcept {
    write_head_.store(0, std::memory_order_release);
    count_.store(0, std::memory_order_release);
  }

  /// Push one sample.  Overwrites the oldest sample when full.
  void Push(uint64_t timestamp_us, float value) noexcept {
    const size_t head = write_head_.load(std::memory_order_relaxed);
    buffer_[head & kMask] = Sample{timestamp_us, value};
    write_head_.store(head + 1, std::memory_order_release);

    // Saturate count at Capacity
    const size_t prev = count_.load(std::memory_order_relaxed);
    if (prev < Capacity) {
      count_.store(prev + 1, std::memory_order_release);
    }
  }

  /// Copy all stored samples in chronological order into @p out.
  /// Returns the number of samples copied (≤ out.size()).
  size_t ReadAll(std::span<Sample> out) const noexcept {
    const size_t head = write_head_.load(std::memory_order_acquire);
    const size_t n = count_.load(std::memory_order_acquire);
    const size_t copy_count = (n < out.size()) ? n : out.size();

    // Oldest entry is at (head - n) % Capacity
    const size_t oldest = (head - n) & kMask;
    for (size_t i = 0; i < copy_count; ++i) {
      out[i] = buffer_[(oldest + i) & kMask];
    }
    return copy_count;
  }

  /// Number of valid samples currently stored (0 … Capacity).
  [[nodiscard]] size_t Size() const noexcept {
    return count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool Empty() const noexcept {
    return Size() == 0;
  }

 private:
  static constexpr size_t kMask = Capacity - 1;

  std::array<Sample, Capacity> buffer_{};
  std::atomic<size_t> write_head_{0};
  std::atomic<size_t> count_{0};
};

}  // namespace cymon
