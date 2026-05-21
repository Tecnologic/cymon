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
  transport_.SubscribeRequest(&sub_get_info_, kGetInfoServiceId,
                              /*extent=*/0, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_us, &CyphalTransport::kDispatchVtable);
  transport_.SubscribeRequest(&sub_reg_access_, kRegisterAccessServiceId,
                              /*extent=*/264, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_us, &CyphalTransport::kDispatchVtable);

  transport_.AddRxCallback([this](const CyphalTransfer& t) {
    if (t.kind == canard_kind_request) {
      if (t.port_id == kGetInfoServiceId) {
        HandleGetInfo(t);
      } else if (t.port_id == kRegisterAccessServiceId) {
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
  // Total = 5 bytes
  const uint32_t uptime_s = static_cast<uint32_t>(now_us / 1000000u);

  uint8_t payload[5]{};
  payload[0] = static_cast<uint8_t>(uptime_s & 0xFFu);
  payload[1] = static_cast<uint8_t>((uptime_s >> 8) & 0xFFu);
  payload[2] = static_cast<uint8_t>((uptime_s >> 16) & 0xFFu);
  payload[3] = static_cast<uint8_t>((uptime_s >> 24) & 0xFFu);
  payload[4] = 0u;  // health=NOMINAL, mode=OPERATIONAL, vssc=0

  const canard_us_t deadline = static_cast<canard_us_t>(now_us) + 1000000;
  transport_.Publish13b(deadline, canard_prio_slow, kHeartbeatSubjectId, transfer_id_++, payload, sizeof(payload));
  transport_.Poll();
}

// ---------------------------------------------------------------------------
// HandleGetInfo  (uavcan.node.GetInfo.1.0 response)
// ---------------------------------------------------------------------------
void CyphalNode::HandleGetInfo(const CyphalTransfer& t) {
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

  const canard_us_t deadline = static_cast<canard_us_t>(esp_timer_get_time()) + 1000000;
  transport_.Respond(deadline, canard_prio_nominal, kGetInfoServiceId, t.source_node_id, t.transfer_id, resp, offset);
  transport_.Poll();
}

// ---------------------------------------------------------------------------
// HandleRegisterAccess
// ---------------------------------------------------------------------------
void CyphalNode::HandleRegisterAccess(const CyphalTransfer& t) {
  // Monitor does not expose registers — respond with empty (not present)
  uint8_t resp[2]{0x00, 0x00};  // mutable=0, persistent=0, value tag=empty(0)

  const canard_us_t deadline = static_cast<canard_us_t>(esp_timer_get_time()) + 1000000;
  transport_.Respond(deadline, canard_prio_nominal, kRegisterAccessServiceId, t.source_node_id, t.transfer_id, resp, sizeof(resp));
  transport_.Poll();
}

}  // namespace cymon
