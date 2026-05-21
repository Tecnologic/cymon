#include "cyphal/timesync.hpp"

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.TIME";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
TimeSyncProvider::TimeSyncProvider(CyphalTransport& transport)
    : transport_(transport), last_master_seen_us_(static_cast<uint64_t>(esp_timer_get_time())) {
  transport_.Subscribe13b(&sub_timesync_, kTimeSyncSubjectId,
                          /*extent=*/7, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_us, &CyphalTransport::kDispatchVtable);

  transport_.AddRxCallback([this](const CyphalTransfer& t) {
    if (t.kind == canard_kind_message_13b && t.port_id == kTimeSyncSubjectId) {
      HandleTimeSync(t);
    }
  });
}

// ---------------------------------------------------------------------------
// HandleTimeSync
// ---------------------------------------------------------------------------
void TimeSyncProvider::HandleTimeSync(const CyphalTransfer& t) {
  if (t.source_node_id != CANARD_NODE_ID_ANONYMOUS) {
    last_master_seen_us_ = static_cast<uint64_t>(t.timestamp_us);
    if (is_master_) {
      CYMON_LOGI(kTag, "External time master detected, stepping down");
      is_master_ = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void TimeSyncProvider::Tick(uint64_t now_us) {
  if (!is_master_) {
    if (now_us - last_master_seen_us_ > kMasterTimeoutUs) {
      CYMON_LOGI(kTag, "No time master — becoming time sync master");
      is_master_ = true;
    }
  }

  if (is_master_ && (now_us - last_publish_us_) >= kPublishIntervalUs) {
    PublishTimeSync(now_us);
    last_publish_us_ = now_us;
  }
}

// ---------------------------------------------------------------------------
// PublishTimeSync  (uavcan.time.Synchronization.1.0)
// ---------------------------------------------------------------------------
void TimeSyncProvider::PublishTimeSync(uint64_t now_us) {
  // uavcan.time.Synchronization.1.0:
  //   previous_transmission_timestamp_microsecond : uint56 (7 bytes)
  uint8_t payload[7]{};
  const uint64_t ts = last_publish_us_;
  for (int i = 0; i < 7; ++i) {
    payload[i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFFu);
  }

  const canard_us_t deadline = static_cast<canard_us_t>(now_us) + 1000000;
  transport_.Publish13b(deadline, canard_prio_immediate, kTimeSyncSubjectId, transfer_id_++, payload, sizeof(payload));
  transport_.Poll();
}

}  // namespace cymon
