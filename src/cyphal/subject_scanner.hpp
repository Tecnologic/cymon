#pragma once

#include <functional>
#include <map>
#include <string>

#include "can/cyphal_transport.hpp"
#include "monitor/node_record.hpp"

namespace cymon {

/// Iterates uavcan.register.List.1.0 on each node to discover what
/// Cyphal subjects a node publishes.  Builds the SubjectInfo list in
/// NodeRecord.
class SubjectScanner {
 public:
  static constexpr CanardPortID kRegisterListServiceId = 385u;

  using DoneCallback = std::function<void(uint8_t node_id, const std::vector<SubjectInfo>&)>;

  SubjectScanner(CyphalTransport& transport, DoneCallback on_done);

  /// Start scanning a node (pages through register.List.1.0 responses).
  void ScanNode(uint8_t node_id);

  /// Timeout / progress check — call at ~1 Hz.
  void Tick(uint64_t now_us);

 private:
  void HandleResponse(const CanardRxTransfer& transfer);
  void RequestNextPage();

  CyphalTransport& transport_;
  DoneCallback on_done_;

  uint8_t pending_node_id_{CANARD_NODE_ID_UNSET};
  uint16_t next_index_{0};
  uint64_t request_deadline_us_{0};
  uint8_t transfer_id_{0};

  std::vector<SubjectInfo> accumulated_;
};

}  // namespace cymon
