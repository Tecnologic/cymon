#include "monitor/monitor_session.hpp"

#include <algorithm>
#include <cmath>

namespace cymon {

// ---------------------------------------------------------------------------
// ConfigureChannels
// ---------------------------------------------------------------------------
void MonitorSession::ConfigureChannels(std::span<const ChannelConfig> configs) noexcept {
  const size_t n = std::min(configs.size(), kMaxSessionChannels);
  num_channels_ = n;
  for (size_t i = 0; i < n; ++i) {
    keys_[i] = configs[i].key;
    rings_[i].Clear();
    prev_values_[i] = 0.0F;
  }
  // Mark unused slots as invalid
  for (size_t i = n; i < kMaxSessionChannels; ++i) {
    keys_[i] = {0xFF, 0xFF};
  }
  armed_ = false;
  triggered_ = false;
  post_trigger_samples_ = 0;
}

// ---------------------------------------------------------------------------
// ConfigureTrigger
// ---------------------------------------------------------------------------
void MonitorSession::ConfigureTrigger(const TriggerConfig& cfg) noexcept {
  trigger_cfg_ = cfg;
}

// ---------------------------------------------------------------------------
// FeedSample
// ---------------------------------------------------------------------------
void MonitorSession::FeedSample(uint8_t node_id, uint8_t variable_id, uint64_t timestamp_us, float value) noexcept {
  const int idx = FindChannel(node_id, variable_id);
  if (idx < 0) {
    return;
  }

  if (mode_ == SessionMode::kTriggered) {
    if (!armed_) {
      return;
    }
    if (triggered_) {
      // Post-trigger capture
      rings_[static_cast<size_t>(idx)].Push(timestamp_us, value);
      if (static_cast<size_t>(idx) == trigger_cfg_.channel_index) {
        ++post_trigger_samples_;
        // Freeze when enough post-trigger samples accumulated (best effort)
        if (post_trigger_samples_ >= kDefaultRingDepth / 2) {
          armed_ = false;
        }
      }
    } else {
      // Pre-trigger: keep rolling; check edge
      rings_[static_cast<size_t>(idx)].Push(timestamp_us, value);
      const float prev = prev_values_[static_cast<size_t>(idx)];
      CheckTrigger(static_cast<size_t>(idx), prev, value, timestamp_us);
    }
  } else {
    // Rolling — always push
    rings_[static_cast<size_t>(idx)].Push(timestamp_us, value);
  }

  prev_values_[static_cast<size_t>(idx)] = value;
}

// ---------------------------------------------------------------------------
// GetGraphData
// ---------------------------------------------------------------------------
void MonitorSession::GetGraphData(GraphDataResponse& response) const noexcept {
  response.session_id = session_id_;
  response.num_channels = static_cast<uint8_t>(num_channels_);

  for (size_t i = 0; i < num_channels_; ++i) {
    ChannelSnapshot& snap = response.channels[i];
    snap.key = keys_[i];
    snap.count = rings_[i].ReadAll(std::span<Sample>{snap.samples.data(), snap.samples.size()});
  }
}

// ---------------------------------------------------------------------------
// FindChannel
// ---------------------------------------------------------------------------
int MonitorSession::FindChannel(uint8_t node_id, uint8_t variable_id) const noexcept {
  for (size_t i = 0; i < num_channels_; ++i) {
    if (keys_[i].node_id == node_id && keys_[i].variable_id == variable_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// CheckTrigger
// ---------------------------------------------------------------------------
void MonitorSession::CheckTrigger(size_t ch_index, float prev, float current, uint64_t /*timestamp_us*/) noexcept {
  if (ch_index != trigger_cfg_.channel_index) {
    return;
  }
  const float lvl = trigger_cfg_.level;
  bool fired = false;
  if (trigger_cfg_.rising_edge) {
    fired = (prev < lvl) && (current >= lvl);
  } else {
    fired = (prev > lvl) && (current <= lvl);
  }
  if (fired) {
    triggered_ = true;
    post_trigger_samples_ = 0;
  }
}

}  // namespace cymon
