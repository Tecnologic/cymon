#include "can/cyphal_transport.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

namespace cymon {

static constexpr const char* kTag = "CYMON.CYP";

// ---------------------------------------------------------------------------
// Allocator callbacks
// ---------------------------------------------------------------------------
void* CyphalTransport::CanardAllocate(CanardInstance* ins, size_t amount) {
  auto* self = static_cast<CyphalTransport*>(ins->user_reference);
  return o1heapAllocate(self->heap_, amount);
}

void CyphalTransport::CanardFree(CanardInstance* ins, void* pointer) {
  auto* self = static_cast<CyphalTransport*>(ins->user_reference);
  o1heapFree(self->heap_, pointer);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
CyphalTransport::CyphalTransport(uint8_t node_id) {
  heap_ = o1heapInit(heap_arena_, kHeapSize);
  if (heap_ == nullptr) {
    CYMON_LOGE(kTag, "o1heap init failed");
    return;
  }

  canard_ = canardInit(CanardAllocate, CanardFree);
  canard_.node_id = node_id;
  canard_.user_reference = this;

  tx_queue_ = canardTxInit(64, CANARD_MTU_CAN_FD);
  CYMON_LOGI(kTag, "Cyphal transport init, node_id=%u", node_id);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
CyphalTransport::~CyphalTransport() {
  // Pop and free all pending TX frames
  const CanardTxQueueItem* item = canardTxPeek(&tx_queue_);
  while (item != nullptr) {
    canard_.memory.deallocate(&canard_, canardTxPop(&tx_queue_, item));
    item = canardTxPeek(&tx_queue_);
  }
}

// ---------------------------------------------------------------------------
// Subscribe
// ---------------------------------------------------------------------------
CanardRxSubscription* CyphalTransport::Subscribe(CanardTransferKind kind, CanardPortID port_id, size_t extent,
                                                 CanardMicrosecond transfer_id_timeout_us) {
  auto* sub = static_cast<CanardRxSubscription*>(o1heapAllocate(heap_, sizeof(CanardRxSubscription)));
  if (sub == nullptr) {
    CYMON_LOGE(kTag, "OOM subscribing port %u", port_id);
    return nullptr;
  }
  if (canardRxSubscribe(&canard_, kind, port_id, extent, transfer_id_timeout_us, sub) < 0) {
    CYMON_LOGE(kTag, "canardRxSubscribe failed port %u", port_id);
    o1heapFree(heap_, sub);
    return nullptr;
  }
  return sub;
}

// ---------------------------------------------------------------------------
// Unsubscribe
// ---------------------------------------------------------------------------
bool CyphalTransport::Unsubscribe(CanardTransferKind kind, CanardPortID port_id) {
  return canardRxUnsubscribe(&canard_, kind, port_id) > 0;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------
int32_t CyphalTransport::Transmit(const CanardTransferMetadata& meta, size_t payload_size, const void* payload) {
  const CanardMicrosecond deadline = static_cast<CanardMicrosecond>(esp_timer_get_time()) + 100000u;
  const int32_t result = canardTxPush(&tx_queue_, &canard_, deadline, &meta, payload_size, payload);
  if (result < 0) {
    CYMON_LOGW(kTag, "canardTxPush error %d", result);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Push (inbound frame)
// ---------------------------------------------------------------------------
void CyphalTransport::Push(const CanFrame& frame) {
  CanardFrame cf{};
  cf.extended_can_id = frame.id;
  cf.payload_size = frame.dlc;
  cf.payload = frame.data;

  CanardRxTransfer transfer{};
  CanardRxSubscription* sub = nullptr;

  const int8_t result = canardRxAccept(&canard_, static_cast<CanardMicrosecond>(frame.timestamp_us), &cf, 0, &transfer, &sub);
  if (result == 1) {
    if (rx_callback_) {
      rx_callback_(transfer);
    }
    canard_.memory.deallocate(&canard_, transfer.payload);
  }
}

// ---------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------
void CyphalTransport::Drain(const SendFn& send_fn, CanardMicrosecond now_us) {
  const CanardTxQueueItem* item = canardTxPeek(&tx_queue_);
  while (item != nullptr) {
    if (item->tx_deadline_usec < now_us) {
      // Expired — discard
      canard_.memory.deallocate(&canard_, canardTxPop(&tx_queue_, item));
      item = canardTxPeek(&tx_queue_);
      continue;
    }

    CanFrame cf{};
    cf.id = item->frame.extended_can_id;
    cf.dlc = static_cast<uint8_t>(item->frame.payload_size);
    cf.fd_frame = true;
    cf.brs = true;
    cf.extended_id = true;
    std::memcpy(cf.data, item->frame.payload, cf.dlc);

    if (!send_fn(cf)) {
      break;  // TX FIFO full — retry next call
    }
    canard_.memory.deallocate(&canard_, canardTxPop(&tx_queue_, item));
    item = canardTxPeek(&tx_queue_);
  }
}

}  // namespace cymon
