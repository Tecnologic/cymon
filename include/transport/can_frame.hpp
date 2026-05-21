#pragma once

#include <cstdint>

namespace cymon {

/// Platform-agnostic CAN-FD frame representation.
/// Used as the interchange type between the MCP2518FD driver and the
/// Cyphal transport layer.
struct CanFrame {
  /// Extended 29-bit CAN identifier (bits [28:0]).
  /// Bit 29 is unused.  Bit 30 = RTR (always 0 for CAN-FD).
  /// Bit 31 = FD format flag.
  uint32_t id{0};

  /// Payload bytes.  CAN-FD max payload = 64 bytes.
  uint8_t data[64]{};

  /// Actual payload length in bytes (0–64).
  uint8_t dlc{0};

  /// True if this frame uses CAN-FD bit-rate switching.
  bool brs{false};

  /// True if this is a CAN-FD (not classic CAN 2.0) frame.
  bool fd_frame{true};

  /// True if this is an extended-format frame (29-bit ID).
  bool extended_id{true};

  /// Timestamp in microseconds at which the frame was received/sent.
  /// On the target this is filled from esp_timer_get_time().
  uint64_t timestamp_us{0};

  static constexpr uint32_t kExtendedIdMask = 0x1FFFFFFFu;
};

}  // namespace cymon
