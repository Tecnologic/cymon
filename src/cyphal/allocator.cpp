#include "cyphal/allocator.hpp"

#include <cstring>

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.ALLOC";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
PnpAllocator::PnpAllocator(CyphalTransport& transport, uint8_t local_node_id)
    : transport_(transport), local_node_id_(local_node_id), startup_us_(static_cast<uint64_t>(esp_timer_get_time())) {
  transport_.Subscribe(CanardTransferKindMessage, kPnpSubjectId,
                       /*extent=*/19, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

  transport_.AddRxCallback([this](const CanardRxTransfer& t) {
    if (t.metadata.transfer_kind == CanardTransferKindMessage && t.metadata.port_id == kPnpSubjectId) {
      HandleAllocationData(t);
    }
  });
  CYMON_LOGI(kTag, "PnP allocator in passive mode, waiting %u s", 3u);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void PnpAllocator::Tick(uint64_t now_us) {
  if (active_) {
    return;
  }
  if (!other_allocator_seen_ && (now_us - startup_us_) > kPassiveWindowUs) {
    active_ = true;
    CYMON_LOGI(kTag, "No other allocator found — becoming PnP allocator");
  }
}

// ---------------------------------------------------------------------------
// HandleAllocationData  (uavcan.pnp.NodeIDAllocationData.2.0)
// ---------------------------------------------------------------------------
void PnpAllocator::HandleAllocationData(const CanardRxTransfer& transfer) {
  // If the transfer comes from a known node, it's an allocator response
  if (transfer.metadata.remote_node_id != CANARD_NODE_ID_UNSET) {
    if (!active_) {
      other_allocator_seen_ = true;
      CYMON_LOGI(kTag, "Another allocator (node %u) detected", transfer.metadata.remote_node_id);
    }
    return;
  }

  if (!active_) {
    return;
  }

  // Parse unique_id from the message payload
  // NodeIDAllocationData.2.0: { unique_id: uint8[16] }
  if (transfer.payload_size < 16) {
    return;
  }
  const auto* uid = static_cast<const uint8_t*>(transfer.payload);
  const uint8_t allocated_id = AllocateNodeId(uid);

  CYMON_LOGI(kTag, "Allocating node_id=%u for UID[0..7]=%02x%02x...", allocated_id, uid[0], uid[1]);
  SendAllocationResponse(uid, allocated_id);
}

// ---------------------------------------------------------------------------
// AllocateNodeId
// ---------------------------------------------------------------------------
uint8_t PnpAllocator::AllocateNodeId(const uint8_t* unique_id_16) {
  uint64_t key = 0;
  std::memcpy(&key, unique_id_16, sizeof(key));

  auto it = allocation_table_.find(key);
  if (it != allocation_table_.end()) {
    return it->second;
  }

  // Find a free ID, skipping the monitor's own node-ID and already-allocated IDs
  while (next_dynamic_id_ <= 127) {
    if (next_dynamic_id_ == local_node_id_) {
      ++next_dynamic_id_;
      continue;
    }
    bool used = false;
    for (const auto& [k, v] : allocation_table_) {
      if (v == next_dynamic_id_) {
        used = true;
        break;
      }
    }
    if (!used) {
      break;
    }
    ++next_dynamic_id_;
  }

  const uint8_t id = next_dynamic_id_++;
  allocation_table_[key] = id;
  return id;
}

// ---------------------------------------------------------------------------
// SendAllocationResponse
// ---------------------------------------------------------------------------
void PnpAllocator::SendAllocationResponse(const uint8_t* unique_id, uint8_t node_id) {
  // NodeIDAllocationData.2.0 response: { allocated_node_id: uint16, unique_id: uint8[16] }
  uint8_t payload[18]{};
  payload[0] = node_id;
  payload[1] = 0;
  std::memcpy(payload + 2, unique_id, 16);

  CanardTransferMetadata meta{};
  meta.priority = CanardPriorityNominal;
  meta.transfer_kind = CanardTransferKindMessage;
  meta.port_id = kPnpSubjectId;
  meta.remote_node_id = CANARD_NODE_ID_UNSET;
  meta.transfer_id = transfer_id_++;

  transport_.Transmit(meta, sizeof(payload), payload);
}

}  // namespace cymon
