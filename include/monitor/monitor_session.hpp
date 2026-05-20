#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "monitor/ring_buffer.hpp"

namespace cymon {

/// Maximum channels per session (compile-time limit).
inline constexpr size_t kMaxSessionChannels = 8;

/// Default ring-buffer depth (power of two).
inline constexpr size_t kDefaultRingDepth = 1024;

/// Identifies the channel within a session.
struct ChannelKey {
  uint8_t node_id{0};
  uint8_t variable_id{0};

  [[nodiscard]] bool operator==(const ChannelKey& o) const noexcept {
    return node_id == o.node_id && variable_id == o.variable_id;
  }
};

/// Configuration for a single monitor channel.
struct ChannelConfig {
  ChannelKey key{};
  std::string_view label{};  ///< optional display label
};

/// Trigger configuration (edge trigger).
struct TriggerConfig {
  uint8_t channel_index{0};  ///< which channel is the trigger source
  float level{0.0F};
  bool rising_edge{true};
  uint32_t pre_trigger_us{50000};   ///< microseconds of pre-trigger data to retain
  uint32_t post_trigger_us{50000};  ///< microseconds of post-trigger data to capture
};

/// Operating mode of a session.
enum class SessionMode : uint8_t {
  kRolling = 0,    ///< buffers always fill and wrap
  kTriggered = 1,  ///< freeze after post-trigger depth reached
};

/// Snapshot of one channel's data for delivery to the WebSocket streamer.
struct ChannelSnapshot {
  ChannelKey key{};
  std::array<Sample, kDefaultRingDepth> samples{};
  size_t count{0};
};

/// Full graph data response from one session.
struct GraphDataResponse {
  uint8_t session_id{0};
  std::array<ChannelSnapshot, kMaxSessionChannels> channels{};
  uint8_t num_channels{0};
};

/// A monitor session owns up to kMaxSessionChannels ring buffers.
///
/// Thread-safety: ConfigureChannels() and ConfigureTrigger() are NOT
/// reentrant with Push/FeedSample.  Callers must ensure configuration
/// is not changed while samples are being pushed concurrently.  On the
/// target this is enforced by draining the Cyphal RX task before
/// reconfiguring.  In host tests no concurrent pushes happen.
class MonitorSession {
 public:
  explicit MonitorSession(uint8_t session_id) : session_id_(session_id) {}

  /// Assign channels and clear all buffers.
  /// @param configs  span of up to kMaxSessionChannels entries; excess ignored.
  void ConfigureChannels(std::span<const ChannelConfig> configs) noexcept;

  /// Change the trigger configuration without clearing buffers.
  void ConfigureTrigger(const TriggerConfig& cfg) noexcept;

  /// Set operating mode.
  void SetMode(SessionMode mode) noexcept { mode_ = mode; }

  /// Feed one sample.  If the channel is not registered this is a no-op.
  void FeedSample(uint8_t node_id, uint8_t variable_id, uint64_t timestamp_us, float value) noexcept;

  /// Populate @p response with a snapshot of all channel data.
  void GetGraphData(GraphDataResponse& response) const noexcept;

  [[nodiscard]] uint8_t Id() const noexcept { return session_id_; }
  [[nodiscard]] SessionMode Mode() const noexcept { return mode_; }
  [[nodiscard]] uint8_t NumChannels() const noexcept { return static_cast<uint8_t>(num_channels_); }

  /// True if the session is "armed" and waiting for or capturing a trigger.
  [[nodiscard]] bool IsArmed() const noexcept { return armed_; }

  /// Arm the trigger (only meaningful in kTriggered mode).
  void Arm() noexcept {
    armed_ = true;
    triggered_ = false;
    post_trigger_samples_ = 0;
  }

  /// Disarm without freezing.
  void Disarm() noexcept { armed_ = false; }

 private:
  uint8_t session_id_{0};
  SessionMode mode_{SessionMode::kRolling};
  TriggerConfig trigger_cfg_{};
  bool armed_{false};
  bool triggered_{false};
  uint32_t post_trigger_samples_{0};

  size_t num_channels_{0};
  std::array<ChannelKey, kMaxSessionChannels> keys_{};

  // One ring buffer per channel — 1024 samples × 12 bytes = 12 KB per channel
  std::array<TimestampedRingBuffer<kDefaultRingDepth>, kMaxSessionChannels> rings_{};

  // Previous value per channel for edge detection
  std::array<float, kMaxSessionChannels> prev_values_{};

  [[nodiscard]] int FindChannel(uint8_t node_id, uint8_t variable_id) const noexcept;
  void CheckTrigger(size_t ch_index, float prev, float current, uint64_t timestamp_us) noexcept;
};

}  // namespace cymon
