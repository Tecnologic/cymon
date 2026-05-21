#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cymon {

/// Health status mirroring uavcan.node.Health.1.0
enum class NodeHealth : uint8_t {
  kNominal = 0,
  kAdvisory = 1,
  kCaution = 2,
  kWarning = 3,
  kOffline = 255,  ///< synthetic — node missed heartbeat timeout
};

/// A variable exposed by a remote device node.
struct VariableInfo {
  uint16_t variable_id{0};
  std::string name{};
  std::string unit{};
};

/// A Cyphal subject published by a remote device node.
struct SubjectInfo {
  uint16_t subject_id{0};
  std::string data_type{};
};

/// Runtime record for a discovered Cyphal node.
struct NodeRecord {
  uint8_t node_id{0};
  std::string name{};
  NodeHealth health{NodeHealth::kOffline};
  uint64_t last_heartbeat_us{0};  ///< esp_timer_get_time() at last heartbeat
  std::vector<VariableInfo> variables{};
  std::vector<SubjectInfo> subjects{};

  [[nodiscard]] bool IsOnline() const {
    return health != NodeHealth::kOffline;
  }
};

}  // namespace cymon
