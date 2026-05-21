#include "cyphal/scanner.hpp"

#include "esp_log.h"

namespace cymon {

static constexpr const char* kTag = "CYMON.SCAN";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
NetworkScanner::NetworkScanner(CyphalTransport& transport, NodeChangedCallback on_change)
    : transport_(transport), on_change_(std::move(on_change)) {
  transport_.Subscribe(CanardTransferKindMessage, kHeartbeatSubjectId,
                       /*extent=*/7, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

  transport_.SetRxCallback([this](const CanardRxTransfer& t) {
    if (t.metadata.transfer_kind == CanardTransferKindMessage && t.metadata.port_id == kHeartbeatSubjectId) {
      HandleHeartbeat(t);
    }
  });
}

// ---------------------------------------------------------------------------
// HandleHeartbeat  (uavcan.node.Heartbeat.1.0)
// ---------------------------------------------------------------------------
void NetworkScanner::HandleHeartbeat(const CanardRxTransfer& transfer) {
  const uint8_t node_id = static_cast<uint8_t>(transfer.metadata.remote_node_id);
  if (node_id == CANARD_NODE_ID_UNSET) {
    return;
  }

  const uint8_t* payload = static_cast<const uint8_t*>(transfer.payload);
  const uint8_t health_raw = (transfer.payload_size >= 5) ? (payload[4] & 0x03u) : 0u;

  auto& rec = nodes_[node_id];
  rec.node_id = node_id;
  rec.last_heartbeat_us = static_cast<uint64_t>(transfer.timestamp_usec);

  const NodeHealth prev_health = rec.health;
  rec.health = static_cast<NodeHealth>(health_raw);

  if (rec.health != prev_health) {
    CYMON_LOGI(kTag, "Node %u health changed: %u", node_id, static_cast<uint8_t>(rec.health));
    if (on_change_) {
      on_change_(rec);
    }
  }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void NetworkScanner::Tick(uint64_t now_us) {
  for (auto& [id, rec] : nodes_) {
    if (rec.health != NodeHealth::kOffline && (now_us - rec.last_heartbeat_us) > kOfflineTimeoutUs) {
      CYMON_LOGW(kTag, "Node %u went offline", id);
      rec.health = NodeHealth::kOffline;
      if (on_change_) {
        on_change_(rec);
      }
    }
  }
}

}  // namespace cymon
