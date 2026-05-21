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
  transport_.Subscribe(CanardTransferKindMessage, kTimeSyncSubjectId,
                       /*extent=*/7, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

  transport_.SetRxCallback([this](const CanardRxTransfer& t) {
    if (t.metadata.transfer_kind == CanardTransferKindMessage && t.metadata.port_id == kTimeSyncSubjectId) {
      HandleTimeSync(t);
    }
  });
}

// ---------------------------------------------------------------------------
// HandleTimeSync
// ---------------------------------------------------------------------------
void TimeSyncProvider::HandleTimeSync(const CanardRxTransfer& transfer) {
  if (transfer.metadata.remote_node_id != CANARD_NODE_ID_UNSET) {
    last_master_seen_us_ = static_cast<uint64_t>(transfer.timestamp_usec);
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
  //   previous_transmission_timestamp_microsecond : uint56
  // 7 bytes total.

  uint8_t payload[7]{};
  // previous_transmission_timestamp is the timestamp of the PREVIOUS publish
  const uint64_t ts = last_publish_us_;
  for (int i = 0; i < 7; ++i) {
    payload[i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFFu);
  }

  CanardTransferMetadata meta{};
  meta.priority = CanardPriorityRealTime;
  meta.transfer_kind = CanardTransferKindMessage;
  meta.port_id = kTimeSyncSubjectId;
  meta.remote_node_id = CANARD_NODE_ID_UNSET;
  meta.transfer_id = transfer_id_++;

  transport_.Transmit(meta, sizeof(payload), payload);
  (void)now_us;
}

}  // namespace cymon
