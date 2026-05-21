#pragma once

#include "can/cyphal_transport.hpp"

namespace cymon {

/// uavcan.time.Synchronization.1.0 provider.
///
/// Monitors subject-ID 7510 for an existing time master.  If no master is
/// seen for kMasterTimeoutUs, this node begins publishing time synchronisation
/// frames at 1 Hz using esp_timer_get_time() (µs).
///
/// When WiFi is up and SNTP has synced, the time base is adjusted to wall
/// time.
class TimeSyncProvider {
 public:
  static constexpr CanardPortID kTimeSyncSubjectId = 7510u;
  static constexpr uint64_t kMasterTimeoutUs = 5000000u;    // 5 s
  static constexpr uint64_t kPublishIntervalUs = 1000000u;  // 1 Hz

  explicit TimeSyncProvider(CyphalTransport& transport);

  void Tick(uint64_t now_us);

  [[nodiscard]] bool IsMaster() const {
    return is_master_;
  }

 private:
  void HandleTimeSync(const CanardRxTransfer& transfer);
  void PublishTimeSync(uint64_t now_us);

  CyphalTransport& transport_;
  bool is_master_{false};
  uint64_t last_master_seen_us_{0};
  uint64_t last_publish_us_{0};
  uint8_t transfer_id_{0};
};

}  // namespace cymon
