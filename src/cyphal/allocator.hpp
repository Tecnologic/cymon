#pragma once

#include <map>

#include "can/cyphal_transport.hpp"

namespace cymon {

/// Plug-and-play node-ID allocator (uavcan.pnp.NodeIDAllocationData.2.0).
///
/// Monitors the network for 3 s after startup.  If no other allocator is
/// seen, activates and starts allocating node-IDs from a NVS-backed map.
///
/// Subject-ID 8166 (fixed per UAVCAN specification).
class PnpAllocator {
 public:
  static constexpr CanardPortID kPnpSubjectId = 8166u;
  static constexpr uint64_t kPassiveWindowUs = 3000000u;  // 3 s

  explicit PnpAllocator(CyphalTransport& transport);

  void Tick(uint64_t now_us);

  [[nodiscard]] bool IsActive() const { return active_; }

 private:
  void HandleAllocationData(const CanardRxTransfer& transfer);
  void SendAllocationResponse(const uint8_t* unique_id, uint8_t node_id);

  [[nodiscard]] uint8_t AllocateNodeId(const uint8_t* unique_id_16);

  CyphalTransport& transport_;
  bool active_{false};
  bool other_allocator_seen_{false};
  uint64_t startup_us_{0};
  uint8_t transfer_id_{0};

  // unique_id (truncated to 8 bytes as key) → allocated node_id
  std::map<uint64_t, uint8_t> allocation_table_;
  uint8_t next_dynamic_id_{2};  // 1 is reserved for the monitor itself
};

}  // namespace cymon
