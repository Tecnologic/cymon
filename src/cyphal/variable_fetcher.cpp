#include "cyphal/variable_fetcher.hpp"

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.VARF";
static constexpr uint64_t kRequestTimeoutUs = 500000u;  // 500 ms

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
VariableFetcher::VariableFetcher(CyphalTransport& transport, DoneCallback on_done) : transport_(transport), on_done_(std::move(on_done)) {
  transport_.SubscribeResponse(&sub_response_, kGetVariableListServiceId,
                               /*extent=*/512, &CyphalTransport::kDispatchVtable);

  transport_.AddRxCallback([this](const CyphalTransfer& t) {
    if (t.kind == canard_kind_response && t.port_id == kGetVariableListServiceId) {
      HandleResponse(t);
    }
  });
}

// ---------------------------------------------------------------------------
// RequestFrom
// ---------------------------------------------------------------------------
void VariableFetcher::RequestFrom(uint8_t node_id) {
  if (pending_node_id_ != CANARD_NODE_ID_ANONYMOUS) {
    return;
  }
  pending_node_id_ = node_id;
  request_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + kRequestTimeoutUs;

  const canard_us_t deadline = static_cast<canard_us_t>(esp_timer_get_time()) + static_cast<canard_us_t>(kRequestTimeoutUs);
  transport_.Request(deadline, canard_prio_nominal, kGetVariableListServiceId, node_id, transfer_id_++, nullptr, 0);
  transport_.Poll();
  CYMON_LOGD(kTag, "Requested variable list from node %u", node_id);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void VariableFetcher::Tick(uint64_t now_us) {
  if (pending_node_id_ != CANARD_NODE_ID_ANONYMOUS && now_us > request_deadline_us_) {
    CYMON_LOGW(kTag, "Variable list request to node %u timed out", pending_node_id_);
    pending_node_id_ = CANARD_NODE_ID_ANONYMOUS;
  }
}

// ---------------------------------------------------------------------------
// HandleResponse
// ---------------------------------------------------------------------------
void VariableFetcher::HandleResponse(const CyphalTransfer& t) {
  const uint8_t node_id = t.source_node_id;
  if (node_id != pending_node_id_) {
    return;
  }
  pending_node_id_ = CANARD_NODE_ID_ANONYMOUS;

  // Parse variable list from payload.
  // Format (provisional, matches cymon-lib GetVariableList.1.0 response):
  //   uint8_t  count
  //   [count × { uint16_t variable_id, uint8_t name_len, char name[], uint8_t unit_len, char unit[] }]
  std::vector<VariableInfo> vars;
  const auto* p = static_cast<const uint8_t*>(t.payload);
  const auto* end = p + t.payload_size;

  if (p >= end) {
    CYMON_LOGW(kTag, "Empty variable list response from node %u", node_id);
    if (on_done_)
      on_done_(node_id, vars);
    return;
  }

  const uint8_t count = *p++;
  for (uint8_t i = 0; i < count && p < end; ++i) {
    if (p + 2 > end)
      break;
    const uint16_t var_id = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2;

    if (p >= end)
      break;
    const uint8_t name_len = *p++;
    if (p + name_len > end)
      break;
    std::string name(reinterpret_cast<const char*>(p), name_len);
    p += name_len;

    if (p >= end)
      break;
    const uint8_t unit_len = *p++;
    if (p + unit_len > end)
      break;
    std::string unit(reinterpret_cast<const char*>(p), unit_len);
    p += unit_len;

    vars.push_back({var_id, std::move(name), std::move(unit)});
  }

  CYMON_LOGI(kTag, "Got %zu variables from node %u", vars.size(), node_id);
  if (on_done_) {
    on_done_(node_id, vars);
  }
}

}  // namespace cymon
