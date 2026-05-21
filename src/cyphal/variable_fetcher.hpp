#pragma once

#include <functional>
#include <map>

#include "can/cyphal_transport.hpp"
#include "monitor/node_record.hpp"

namespace cymon {

/// Queries each discovered node for its variable list using the
/// cymon.GetVariableList.1.0 service (subject-ID TBD once cymon-lib is
/// integrated; using 512 as a placeholder).
class VariableFetcher {
 public:
  static constexpr uint16_t kGetVariableListServiceId = 512u;  // placeholder

  using DoneCallback = std::function<void(uint8_t node_id, const std::vector<VariableInfo>&)>;

  VariableFetcher(CyphalTransport& transport, DoneCallback on_done);

  /// Kick off a fetch for @p node_id.  No-op if a fetch is already in flight.
  void RequestFrom(uint8_t node_id);

  /// Timeout check — call from scanner_task at ~1 Hz.
  void Tick(uint64_t now_us);

 private:
  void HandleResponse(const CyphalTransfer& t);

  CyphalTransport& transport_;
  DoneCallback on_done_;
  uint8_t pending_node_id_{CANARD_NODE_ID_ANONYMOUS};
  uint64_t request_deadline_us_{0};
  uint_least8_t transfer_id_{0};

  canard_subscription_t sub_response_{};
};

}  // namespace cymon
