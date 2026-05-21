#include "cyphal/node.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.NODE";
static constexpr uint64_t kHeartbeatIntervalUs = 1000000u;  // 1 Hz

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
CyphalNode::CyphalNode(CyphalTransport& transport, const NodeInfo& info) : transport_(transport), info_(info) {
  // Subscribe to GetInfo requests
  transport_.Subscribe(CanardTransferKindRequest, kGetInfoServiceId,
                       /*extent=*/0, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);
  // Subscribe to register.Access requests
  transport_.Subscribe(CanardTransferKindRequest, kRegisterAccessServiceId,
                       /*extent=*/264, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

  transport_.SetRxCallback([this](const CanardRxTransfer& t) {
    if (t.metadata.transfer_kind == CanardTransferKindRequest) {
      if (t.metadata.port_id == kGetInfoServiceId) {
        HandleGetInfo(t);
      } else if (t.metadata.port_id == kRegisterAccessServiceId) {
        HandleRegisterAccess(t);
      }
    }
  });

  CYMON_LOGI(kTag, "CyphalNode created: %s", std::string(info_.name).c_str());
}

// ---------------------------------------------------------------------------
// Spin
// ---------------------------------------------------------------------------
void CyphalNode::Spin(uint64_t now_us) {
  if (now_us - last_heartbeat_us_ >= kHeartbeatIntervalUs) {
    PublishHeartbeat(now_us);
    last_heartbeat_us_ = now_us;
  }
}

// ---------------------------------------------------------------------------
// PublishHeartbeat  (uavcan.node.Heartbeat.1.0)
// ---------------------------------------------------------------------------
void CyphalNode::PublishHeartbeat(uint64_t now_us) {
  // uavcan.node.Heartbeat.1.0 serialisation:
  //   uptime       : uint32 (seconds)
  //   health.value : uint2  (0=NOMINAL)
  //   mode.value   : uint3  (0=OPERATIONAL)
  //   vssc         : uint3  (vendor-specific status code)
  // Total = 7 bytes

  const uint32_t uptime_s = static_cast<uint32_t>(now_us / 1000000u);

  uint8_t payload[7]{};
  payload[0] = static_cast<uint8_t>(uptime_s & 0xFFu);
  payload[1] = static_cast<uint8_t>((uptime_s >> 8) & 0xFFu);
  payload[2] = static_cast<uint8_t>((uptime_s >> 16) & 0xFFu);
  payload[3] = static_cast<uint8_t>((uptime_s >> 24) & 0xFFu);
  payload[4] = 0u;  // health=NOMINAL, mode=OPERATIONAL, vssc=0

  CanardTransferMetadata meta{};
  meta.priority = CanardPrioritySlow;
  meta.transfer_kind = CanardTransferKindMessage;
  meta.port_id = kHeartbeatSubjectId;
  meta.remote_node_id = CANARD_NODE_ID_UNSET;
  meta.transfer_id = transfer_id_++;

  transport_.Transmit(meta, sizeof(payload), payload);
}

// ---------------------------------------------------------------------------
// HandleGetInfo  (uavcan.node.GetInfo.1.0 response)
// ---------------------------------------------------------------------------
void CyphalNode::HandleGetInfo(const CanardRxTransfer& transfer) {
  // Minimal GetInfo response — fixed 58-byte response
  uint8_t resp[74]{};
  size_t offset = 0;

  // protocol_version.major/minor
  resp[offset++] = 1;
  resp[offset++] = 0;
  // hardware_version.major/minor
  resp[offset++] = info_.hw_major;
  resp[offset++] = info_.hw_minor;
  // software_version.major/minor
  resp[offset++] = info_.sw_major;
  resp[offset++] = info_.sw_minor;
  // software_vcs_revision_id (uint64)
  for (int i = 0; i < 8; ++i)
    resp[offset++] = 0;
  // unique_id (16 bytes)
  std::memcpy(resp + offset, info_.unique_id, 16);
  offset += 16;
  // name (variable string, max 50 chars + length byte)
  const size_t name_len = std::min(info_.name.size(), static_cast<size_t>(50));
  resp[offset++] = static_cast<uint8_t>(name_len);
  std::memcpy(resp + offset, info_.name.data(), name_len);
  offset += name_len;

  CanardTransferMetadata meta{};
  meta.priority = CanardPriorityNominal;
  meta.transfer_kind = CanardTransferKindResponse;
  meta.port_id = kGetInfoServiceId;
  meta.remote_node_id = transfer.metadata.remote_node_id;
  meta.transfer_id = transfer.metadata.transfer_id;

  transport_.Transmit(meta, offset, resp);
}

// ---------------------------------------------------------------------------
// HandleRegisterAccess
// ---------------------------------------------------------------------------
void CyphalNode::HandleRegisterAccess(const CanardRxTransfer& transfer) {
  // Monitor does not expose registers — respond with empty (not present)
  uint8_t resp[2]{0x00, 0x00};

  // Minimal register.Access.1.0 response: mutable=0, persistent=0, value tag=empty(0)
  CanardTransferMetadata meta{};
  meta.priority = CanardPriorityNominal;
  meta.transfer_kind = CanardTransferKindResponse;
  meta.port_id = kRegisterAccessServiceId;
  meta.remote_node_id = transfer.metadata.remote_node_id;
  meta.transfer_id = transfer.metadata.transfer_id;

  transport_.Transmit(meta, sizeof(resp), resp);
}

}  // namespace cymon
