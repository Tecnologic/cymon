#pragma once

#include <canard.h>
#include <o1heap.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "transport/can_frame.hpp"

namespace cymon {

/// Unified RX transfer descriptor — passed to every rx callback.
struct CyphalTransfer {
  canard_us_t timestamp_us;
  canard_prio_t priority;
  uint_least8_t source_node_id;
  uint_least8_t transfer_id;
  uint16_t port_id;
  canard_kind_t kind;
  const void* payload;  ///< valid only inside the callback invocation
  size_t payload_size;
};

/// Callback invoked when a complete Cyphal transfer is received.
using CyphalRxCallback = std::function<void(const CyphalTransfer&)>;

/// Cyphal transport layer built on libcanard v5 and o1heap.
///
/// Owns a canard_t instance and an o1heap allocator.  Integrates with the
/// MCP2518FD driver:
///  - IngestFrame() feeds a received CAN-FD frame into the Cyphal RX engine.
///  - SetSendFn() registers the low-level transmit function used by the
///    canard vtable's tx() callback.
///  - Poll() drives the TX pipeline (flushes pending outgoing frames).
class CyphalTransport {
 public:
  using SendFn = std::function<bool(const CanFrame&)>;

  /// @param node_id  Cyphal local node ID (1–127)
  explicit CyphalTransport(uint8_t node_id);
  ~CyphalTransport();

  /// Unified RX dispatch vtable — all Subscribe*() calls should use this.
  /// Each completed transfer fires all callbacks registered via AddRxCallback().
  static const canard_subscription_vtable_t kDispatchVtable;

  // Non-copyable
  CyphalTransport(const CyphalTransport&) = delete;
  CyphalTransport& operator=(const CyphalTransport&) = delete;

  /// Register the low-level CAN transmit function.
  /// Must be set before Poll() is first called.
  void SetSendFn(SendFn fn) {
    send_fn_ = std::move(fn);
  }

  // -------------------------------------------------------------------------
  // Subscription helpers (allocate + register a canard_subscription_t)
  // -------------------------------------------------------------------------

  /// Subscribe to a 13-bit Cyphal v1.0 message subject.
  canard_subscription_t* Subscribe13b(canard_subscription_t* sub, uint16_t subject_id, size_t extent, canard_us_t transfer_id_timeout_us,
                                      const canard_subscription_vtable_t* vtable);

  /// Subscribe to a Cyphal v1 service request.
  canard_subscription_t* SubscribeRequest(canard_subscription_t* sub, uint16_t service_id, size_t extent,
                                          canard_us_t transfer_id_timeout_us, const canard_subscription_vtable_t* vtable);

  /// Subscribe to a Cyphal v1 service response.
  canard_subscription_t* SubscribeResponse(canard_subscription_t* sub, uint16_t service_id, size_t extent,
                                           const canard_subscription_vtable_t* vtable);

  /// Unsubscribe a previously registered subscription.
  void Unsubscribe(canard_subscription_t* sub);

  // -------------------------------------------------------------------------
  // Publish / request / respond
  // -------------------------------------------------------------------------

  /// Publish a 13-bit Cyphal v1.0 message.
  bool Publish13b(canard_us_t deadline, canard_prio_t prio, uint16_t subject_id, uint_least8_t transfer_id, const void* payload,
                  size_t payload_size);

  /// Send a Cyphal v1 service request.
  bool Request(canard_us_t deadline, canard_prio_t prio, uint16_t service_id, uint_least8_t server_node_id, uint_least8_t transfer_id,
               const void* payload, size_t payload_size);

  /// Send a Cyphal v1 service response.
  bool Respond(canard_us_t deadline, canard_prio_t prio, uint16_t service_id, uint_least8_t client_node_id, uint_least8_t transfer_id,
               const void* payload, size_t payload_size);

  // -------------------------------------------------------------------------
  // Frame I/O
  // -------------------------------------------------------------------------

  /// Feed a received raw CAN-FD frame into the Cyphal RX engine.
  /// Subscription on_message() callbacks fire synchronously inside this call.
  void IngestFrame(const CanFrame& frame);

  /// Drive the TX pipeline — flush pending outgoing frames via send_fn_.
  /// Call periodically (or after IngestFrame to handle request→response).
  void Poll();

  // -------------------------------------------------------------------------
  // Multi-handler RX dispatch
  // -------------------------------------------------------------------------

  /// Register an additional RX callback.  Every registered callback is
  /// invoked for every completed transfer (regardless of port/kind);
  /// callers should filter by CyphalTransfer::kind / port_id.
  void AddRxCallback(CyphalRxCallback cb) {
    rx_callbacks_.push_back(std::move(cb));
  }

  // -------------------------------------------------------------------------
  // Accessors
  // -------------------------------------------------------------------------

  [[nodiscard]] uint8_t NodeId() const {
    return static_cast<uint8_t>(canard_.node_id);
  }

  [[nodiscard]] O1HeapDiagnostics HeapDiagnostics() const {
    return o1heapGetDiagnostics(heap_);
  }

 private:
  // ---- memory vtable helpers ----
  static void MemFree(canard_mem_t mem, size_t /*sz*/, void* ptr);
  static void* MemAlloc(canard_mem_t mem, size_t sz);
  [[nodiscard]] canard_mem_t MakeMem() const;

  // ---- canard vtable callbacks ----
  static canard_us_t VtNow(const canard_t* self);
  static bool VtTx(canard_t* self, void* user_ctx, canard_us_t deadline, uint_least8_t iface_index, bool fd, uint32_t extended_can_id,
                   canard_bytes_t can_data);

  // ---- subscription dispatch ----
  static void OnMessage(canard_subscription_t* sub, canard_us_t timestamp, canard_prio_t prio, uint_least8_t source_node_id,
                        uint_least8_t transfer_id, canard_payload_t payload);
  void DispatchRx(const canard_subscription_t* sub, canard_us_t timestamp, canard_prio_t prio, uint_least8_t source_node_id,
                  uint_least8_t transfer_id, canard_payload_t payload);

  /// Helper — build a single-fragment canard_bytes_chain_t from a flat buffer.
  static canard_bytes_chain_t ToChain(const void* data, size_t size);

  static constexpr size_t kHeapSize = 16384;
  static constexpr size_t kTxQueueCapacity = 64;

  alignas(O1HEAP_ALIGNMENT) uint8_t heap_arena_[kHeapSize]{};
  O1HeapInstance* heap_{nullptr};

  canard_vtable_t vtable_{};
  canard_t canard_{};

  SendFn send_fn_;
  std::vector<CyphalRxCallback> rx_callbacks_;
};

}  // namespace cymon
