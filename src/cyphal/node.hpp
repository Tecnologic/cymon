#pragma once

#include <cstdint>
#include <string_view>

#include "can/cyphal_transport.hpp"

namespace cymon {

/// Cyphal node — publishes Heartbeat, responds to GetInfo and register.Access.
class CyphalNode {
 public:
  struct NodeInfo {
    uint64_t unique_id[2]{};  ///< 128-bit unique ID (from ESP32-S2 eFuse)
    std::string_view name{"co.tecnologic.cymon"};
    uint8_t hw_major{1};
    uint8_t hw_minor{0};
    uint8_t sw_major{0};
    uint8_t sw_minor{1};
  };

  CyphalNode(CyphalTransport& transport, const NodeInfo& info);

  /// Call periodically from the Cyphal process task (1 Hz).
  void Spin(uint64_t now_us);

 private:
  void PublishHeartbeat(uint64_t now_us);
  void HandleGetInfo(const CanardRxTransfer& transfer);
  void HandleRegisterAccess(const CanardRxTransfer& transfer);

  CyphalTransport& transport_;
  NodeInfo info_;
  uint8_t transfer_id_{0};
  uint64_t last_heartbeat_us_{0};

  // Subject / service IDs per UAVCAN standard
  static constexpr CanardPortID kHeartbeatSubjectId = 7509u;
  static constexpr CanardPortID kGetInfoServiceId = 430u;
  static constexpr CanardPortID kRegisterAccessServiceId = 384u;
};

}  // namespace cymon
