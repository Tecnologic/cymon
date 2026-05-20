#pragma once

#include <functional>
#include <map>

#include "can/cyphal_transport.hpp"
#include "monitor/node_record.hpp"

namespace cymon {

/// Listens to uavcan.node.Heartbeat.1.0, maintains a table of live nodes,
/// and marks nodes OFFLINE after a configurable timeout.
class NetworkScanner {
 public:
  static constexpr uint64_t kOfflineTimeoutUs = 3000000u;  // 3 s

  using NodeChangedCallback = std::function<void(const NodeRecord&)>;

  NetworkScanner(CyphalTransport& transport, NodeChangedCallback on_change);

  /// Call periodically to apply the offline timeout check.
  void Tick(uint64_t now_us);

  /// Read-only access to node table.
  [[nodiscard]] const std::map<uint8_t, NodeRecord>& Nodes() const { return nodes_; }

 private:
  void HandleHeartbeat(const CanardRxTransfer& transfer);

  CyphalTransport& transport_;
  NodeChangedCallback on_change_;
  std::map<uint8_t, NodeRecord> nodes_;

  static constexpr CanardPortID kHeartbeatSubjectId = 7509u;
};

}  // namespace cymon
