#pragma once

#include <canard.h>
#include <o1heap.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "transport/can_frame.hpp"

namespace cymon {

/// Callback invoked when a complete Cyphal transfer is received.
using CyphalRxCallback = std::function<void(const CanardRxTransfer&)>;

/// Cyphal transport layer using libcanard v3 and o1heap.
///
/// Owns a CanardInstance and an o1heap allocator.  Integrates with the
/// MCP2518FD driver by exposing a Push() method (for inbound frames) and
/// a Drain() method (which reads pending TX frames and sends them through
/// a caller-supplied transmit function).
class CyphalTransport {
 public:
  /// @param node_id  Cyphal local node ID (1–127)
  explicit CyphalTransport(uint8_t node_id);
  ~CyphalTransport();

  // Non-copyable
  CyphalTransport(const CyphalTransport&) = delete;
  CyphalTransport& operator=(const CyphalTransport&) = delete;

  /// Subscribe to a subject or service.
  /// @return CanardRxSubscription* (owned by the transport), or nullptr on OOM.
  CanardRxSubscription* Subscribe(CanardTransferKind kind, CanardPortID port_id, size_t extent, CanardMicrosecond transfer_id_timeout_us);

  /// Unsubscribe.
  bool Unsubscribe(CanardTransferKind kind, CanardPortID port_id);

  /// Queue a transfer for transmission.
  /// @return number of frames enqueued (≥0), or <0 on error.
  int32_t Transmit(const CanardTransferMetadata& meta, size_t payload_size, const void* payload);

  /// Feed a received raw CAN-FD frame into the Cyphal receive logic.
  /// Invokes rx_callback_ for completed transfers.
  void Push(const CanFrame& frame);

  /// Drain all pending TX frames through @p send_fn.
  /// @p send_fn receives the raw CAN-FD frame; returns true if sent.
  using SendFn = std::function<bool(const CanFrame&)>;
  void Drain(const SendFn& send_fn, CanardMicrosecond now_us);

  /// Register a callback for received transfers.  Multiple callbacks are
  /// supported; each is invoked for every completed transfer regardless of
  /// port or kind (libcanard's subscription filtering means each callback
  /// only sees transfers for ports it subscribed to via Subscribe()).
  void AddRxCallback(CyphalRxCallback cb) {
    rx_callbacks_.push_back(std::move(cb));
  }

  [[nodiscard]] uint8_t NodeId() const {
    return static_cast<uint8_t>(canard_.node_id);
  }

  /// Allocator stats (for diagnostics)
  [[nodiscard]] O1HeapDiagnostics HeapDiagnostics() const {
    return o1heapGetDiagnostics(heap_);
  }

 private:
  static void* CanardAllocate(CanardInstance* ins, size_t amount);
  static void CanardFree(CanardInstance* ins, void* pointer);

  static constexpr size_t kHeapSize = 8192;

  alignas(O1HEAP_ALIGNMENT) uint8_t heap_arena_[kHeapSize]{};
  O1HeapInstance* heap_{nullptr};

  CanardInstance canard_{};
  CanardTxQueue tx_queue_{};

  std::vector<CyphalRxCallback> rx_callbacks_;
};

}  // namespace cymon
