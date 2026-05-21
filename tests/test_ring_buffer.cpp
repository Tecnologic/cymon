#include <gtest/gtest.h>

#include <array>
#include <numeric>

#include "monitor/ring_buffer.hpp"

using cymon::Sample;
using cymon::TimestampedRingBuffer;

// ---------------------------------------------------------------------------
// Basic push / read
// ---------------------------------------------------------------------------
TEST(RingBuffer, PushAndReadAll) {
  TimestampedRingBuffer<64> buf;
  EXPECT_TRUE(buf.Empty());
  EXPECT_EQ(buf.Size(), 0u);

  buf.Push(1000, 1.0F);
  buf.Push(2000, 2.0F);
  buf.Push(3000, 3.0F);

  EXPECT_EQ(buf.Size(), 3u);

  std::array<Sample, 64> out{};
  const size_t n = buf.ReadAll(out);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(out[0].timestamp_us, 1000u);
  EXPECT_FLOAT_EQ(out[0].value, 1.0F);
  EXPECT_EQ(out[2].timestamp_us, 3000u);
  EXPECT_FLOAT_EQ(out[2].value, 3.0F);
}

// ---------------------------------------------------------------------------
// Capacity limit
// ---------------------------------------------------------------------------
TEST(RingBuffer, CapacityLimit) {
  TimestampedRingBuffer<64> buf;
  for (int i = 0; i < 100; ++i) {
    buf.Push(static_cast<uint64_t>(i * 1000), static_cast<float>(i));
  }
  EXPECT_EQ(buf.Size(), 64u);
}

// ---------------------------------------------------------------------------
// Rolling overwrite — oldest sample is dropped
// ---------------------------------------------------------------------------
TEST(RingBuffer, RollingOverwrite) {
  TimestampedRingBuffer<4> buf;
  buf.Push(1000, 1.0F);
  buf.Push(2000, 2.0F);
  buf.Push(3000, 3.0F);
  buf.Push(4000, 4.0F);
  // Buffer full, now push one more
  buf.Push(5000, 5.0F);

  std::array<Sample, 4> out{};
  const size_t n = buf.ReadAll(out);
  EXPECT_EQ(n, 4u);
  // Oldest should now be the second original sample
  EXPECT_EQ(out[0].timestamp_us, 2000u);
  EXPECT_EQ(out[3].timestamp_us, 5000u);
}

// ---------------------------------------------------------------------------
// Clear resets state
// ---------------------------------------------------------------------------
TEST(RingBuffer, Clear) {
  TimestampedRingBuffer<64> buf;
  buf.Push(1000, 1.0F);
  buf.Clear();
  EXPECT_TRUE(buf.Empty());
  EXPECT_EQ(buf.Size(), 0u);
}

// ---------------------------------------------------------------------------
// ReadAll into undersized span
// ---------------------------------------------------------------------------
TEST(RingBuffer, ReadAllUndersizedSpan) {
  TimestampedRingBuffer<64> buf;
  buf.Push(1000, 1.0F);
  buf.Push(2000, 2.0F);
  buf.Push(3000, 3.0F);

  std::array<Sample, 2> small_out{};
  const size_t n = buf.ReadAll(small_out);
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(small_out[0].timestamp_us, 1000u);
  EXPECT_EQ(small_out[1].timestamp_us, 2000u);
}

// ---------------------------------------------------------------------------
// Chronological order preserved after many pushes
// ---------------------------------------------------------------------------
TEST(RingBuffer, ChronologicalOrder) {
  TimestampedRingBuffer<64> buf;
  for (uint64_t i = 0; i < 64; ++i) {
    buf.Push(i * 100u, static_cast<float>(i));
  }
  std::array<Sample, 64> out{};
  buf.ReadAll(out);
  for (size_t i = 1; i < 64; ++i) {
    EXPECT_LT(out[i - 1].timestamp_us, out[i].timestamp_us);
  }
}
