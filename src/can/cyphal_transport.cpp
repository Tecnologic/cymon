#include "can/cyphal_transport.hpp"

#include <cstring>

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.CYP";

// ---------------------------------------------------------------------------
// Static vtable for the universal RX dispatcher
// ---------------------------------------------------------------------------
const canard_subscription_vtable_t CyphalTransport::kDispatchVtable = {
    .on_message = CyphalTransport::OnMessage,
};

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
void CyphalTransport::MemFree(canard_mem_t mem, size_t /*sz*/, void* ptr) {
  o1heapFree(static_cast<O1HeapInstance*>(mem.context), ptr);
}

void* CyphalTransport::MemAlloc(canard_mem_t mem, size_t sz) {
  return o1heapAllocate(static_cast<O1HeapInstance*>(mem.context), sz);
}

canard_mem_t CyphalTransport::MakeMem() const {
  static const canard_mem_vtable_t kMemVtable = {
      .free = MemFree,
      .alloc = MemAlloc,
  };
  return canard_mem_t{.vtable = &kMemVtable, .context = heap_};
}

// ---------------------------------------------------------------------------
// canard vtable — now / tx
// ---------------------------------------------------------------------------
canard_us_t CyphalTransport::VtNow(const canard_t* /*self*/) {
  return static_cast<canard_us_t>(esp_timer_get_time());
}

bool CyphalTransport::VtTx(canard_t* self, void* /*user_ctx*/, canard_us_t /*deadline*/, uint_least8_t /*iface_index*/, bool fd,
                           uint32_t extended_can_id, canard_bytes_t can_data) {
  auto* transport = static_cast<CyphalTransport*>(self->user_context);
  if (!transport->send_fn_) {
    return false;
  }
  CanFrame cf{};
  cf.id = extended_can_id;
  cf.dlc = static_cast<uint8_t>(can_data.size);
  cf.fd_frame = fd;
  cf.brs = fd;
  cf.extended_id = true;
  if (can_data.size > 0 && can_data.data != nullptr) {
    std::memcpy(cf.data, can_data.data, can_data.size);
  }
  return transport->send_fn_(cf);
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

  vtable_.now = VtNow;
  vtable_.tx = VtTx;
  vtable_.filter = nullptr;  // hardware filtering not used

  const canard_mem_t mem = MakeMem();
  const canard_mem_set_t mem_set{
      .tx_transfer = mem,
      .tx_frame = mem,
      .rx_session = mem,
      .rx_payload = mem,
      .rx_filters = mem,
  };

  if (!canard_new(&canard_, &vtable_, mem_set, kTxQueueCapacity,
                  /*prng_seed=*/static_cast<uint64_t>(esp_timer_get_time()),
                  /*filter_count=*/0)) {
    CYMON_LOGE(kTag, "canard_new failed");
    return;
  }
  canard_.user_context = this;

  if (!canard_set_node_id(&canard_, node_id)) {
    CYMON_LOGW(kTag, "canard_set_node_id failed for id=%u", node_id);
  }

  CYMON_LOGI(kTag, "Cyphal transport init, node_id=%u", node_id);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
CyphalTransport::~CyphalTransport() {
  canard_destroy(&canard_);
}

// ---------------------------------------------------------------------------
// Subscribe helpers
// ---------------------------------------------------------------------------
canard_subscription_t* CyphalTransport::Subscribe13b(canard_subscription_t* sub, uint16_t subject_id, size_t extent,
                                                     canard_us_t transfer_id_timeout_us, const canard_subscription_vtable_t* vtable) {
  return canard_subscribe_13b(&canard_, sub, subject_id, extent, transfer_id_timeout_us, vtable);
}

canard_subscription_t* CyphalTransport::SubscribeRequest(canard_subscription_t* sub, uint16_t service_id, size_t extent,
                                                         canard_us_t transfer_id_timeout_us, const canard_subscription_vtable_t* vtable) {
  return canard_subscribe_request(&canard_, sub, service_id, extent, transfer_id_timeout_us, vtable);
}

canard_subscription_t* CyphalTransport::SubscribeResponse(canard_subscription_t* sub, uint16_t service_id, size_t extent,
                                                          const canard_subscription_vtable_t* vtable) {
  return canard_subscribe_response(&canard_, sub, service_id, extent, vtable);
}

void CyphalTransport::Unsubscribe(canard_subscription_t* sub) {
  canard_unsubscribe(&canard_, sub);
}

// ---------------------------------------------------------------------------
// Publish / request / respond
// ---------------------------------------------------------------------------
canard_bytes_chain_t CyphalTransport::ToChain(const void* data, size_t size) {
  canard_bytes_chain_t chain{};
  chain.bytes.data = data;
  chain.bytes.size = size;
  chain.next = nullptr;
  return chain;
}

bool CyphalTransport::Publish13b(canard_us_t deadline, canard_prio_t prio, uint16_t subject_id, uint_least8_t transfer_id,
                                 const void* payload, size_t payload_size) {
  const canard_bytes_chain_t chain = ToChain(payload, payload_size);
  const bool ok =
      canard_publish_13b(&canard_, deadline, CANARD_IFACE_BITMAP_ALL, prio, subject_id, transfer_id, chain, /*user_context=*/nullptr);
  if (!ok) {
    CYMON_LOGW(kTag, "canard_publish_13b failed subject=%u", subject_id);
  }
  return ok;
}

bool CyphalTransport::Request(canard_us_t deadline, canard_prio_t prio, uint16_t service_id, uint_least8_t server_node_id,
                              uint_least8_t transfer_id, const void* payload, size_t payload_size) {
  const canard_bytes_chain_t chain = ToChain(payload, payload_size);
  const bool ok = canard_request(&canard_, deadline, prio, service_id, server_node_id, transfer_id, chain,
                                 /*user_context=*/nullptr);
  if (!ok) {
    CYMON_LOGW(kTag, "canard_request failed service=%u", service_id);
  }
  return ok;
}

bool CyphalTransport::Respond(canard_us_t deadline, canard_prio_t prio, uint16_t service_id, uint_least8_t client_node_id,
                              uint_least8_t transfer_id, const void* payload, size_t payload_size) {
  const canard_bytes_chain_t chain = ToChain(payload, payload_size);
  const bool ok = canard_respond(&canard_, deadline, prio, service_id, client_node_id, transfer_id, chain,
                                 /*user_context=*/nullptr);
  if (!ok) {
    CYMON_LOGW(kTag, "canard_respond failed service=%u", service_id);
  }
  return ok;
}

// ---------------------------------------------------------------------------
// IngestFrame — feed a received CAN-FD frame into the RX engine
// ---------------------------------------------------------------------------
void CyphalTransport::IngestFrame(const CanFrame& frame) {
  canard_bytes_t can_data{};
  can_data.data = frame.data;
  can_data.size = frame.dlc;

  if (!canard_ingest_frame(&canard_, static_cast<canard_us_t>(frame.timestamp_us),
                           /*iface_index=*/0, frame.id, can_data)) {
    CYMON_LOGW(kTag, "canard_ingest_frame rejected frame id=0x%08x", frame.id);
  }
}

// ---------------------------------------------------------------------------
// Poll — flush pending outgoing frames
// ---------------------------------------------------------------------------
void CyphalTransport::Poll() {
  canard_poll(&canard_, CANARD_IFACE_BITMAP_ALL);
}

// ---------------------------------------------------------------------------
// RX dispatch — static on_message → all registered callbacks
// ---------------------------------------------------------------------------
void CyphalTransport::OnMessage(canard_subscription_t* sub, canard_us_t timestamp, canard_prio_t prio, uint_least8_t source_node_id,
                                uint_least8_t transfer_id, canard_payload_t payload) {
  auto* transport = static_cast<CyphalTransport*>(sub->owner->user_context);
  transport->DispatchRx(sub, timestamp, prio, source_node_id, transfer_id, payload);
  // Free multi-frame payload (single-frame payloads have empty origin)
  if (payload.origin.data != nullptr) {
    canard_mem_t mem = transport->MakeMem();
    mem.vtable->free(mem, payload.origin.size, payload.origin.data);
  }
}

void CyphalTransport::DispatchRx(const canard_subscription_t* sub, canard_us_t timestamp, canard_prio_t prio, uint_least8_t source_node_id,
                                 uint_least8_t transfer_id, canard_payload_t payload) {
  if (rx_callbacks_.empty()) {
    return;
  }
  CyphalTransfer t{};
  t.timestamp_us = timestamp;
  t.priority = prio;
  t.source_node_id = source_node_id;
  t.transfer_id = transfer_id;
  t.port_id = sub->port_id;
  t.kind = sub->kind;
  t.payload = payload.view.data;
  t.payload_size = payload.view.size;

  for (const auto& cb : rx_callbacks_) {
    cb(t);
  }
}

}  // namespace cymon
