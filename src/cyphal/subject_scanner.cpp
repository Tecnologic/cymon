#include "cyphal/subject_scanner.hpp"

#include <cstring>

#include "esp_timer.h"
#include "logging/logger.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.SUBSC";
static constexpr uint64_t kRequestTimeoutUs = 500000u;

// Cyphal register name prefix for subject IDs: "uavcan.pub.<name>.id"
static constexpr std::string_view kPubPrefix = "uavcan.pub.";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
SubjectScanner::SubjectScanner(CyphalTransport& transport, DoneCallback on_done) : transport_(transport), on_done_(std::move(on_done)) {
  transport_.Subscribe(CanardTransferKindResponse, kRegisterListServiceId,
                       /*extent=*/261, CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);

  transport_.SetRxCallback([this](const CanardRxTransfer& t) {
    if (t.metadata.transfer_kind == CanardTransferKindResponse && t.metadata.port_id == kRegisterListServiceId) {
      HandleResponse(t);
    }
  });
}

// ---------------------------------------------------------------------------
// ScanNode
// ---------------------------------------------------------------------------
void SubjectScanner::ScanNode(uint8_t node_id) {
  if (pending_node_id_ != CANARD_NODE_ID_UNSET) {
    return;
  }
  pending_node_id_ = node_id;
  next_index_ = 0;
  accumulated_.clear();
  RequestNextPage();
}

// ---------------------------------------------------------------------------
// RequestNextPage  (uavcan.register.List.1.0 request)
// ---------------------------------------------------------------------------
void SubjectScanner::RequestNextPage() {
  request_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + kRequestTimeoutUs;

  // uavcan.register.List.1.0 request: uint16 index
  uint8_t payload[2];
  payload[0] = static_cast<uint8_t>(next_index_ & 0xFFu);
  payload[1] = static_cast<uint8_t>((next_index_ >> 8) & 0xFFu);

  CanardTransferMetadata meta{};
  meta.priority = CanardPriorityNominal;
  meta.transfer_kind = CanardTransferKindRequest;
  meta.port_id = kRegisterListServiceId;
  meta.remote_node_id = pending_node_id_;
  meta.transfer_id = transfer_id_++;

  transport_.Transmit(meta, sizeof(payload), payload);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void SubjectScanner::Tick(uint64_t now_us) {
  if (pending_node_id_ != CANARD_NODE_ID_UNSET && now_us > request_deadline_us_) {
    CYMON_LOGW(kTag, "register.List page %u from node %u timed out", next_index_, pending_node_id_);
    // Complete with what we have so far
    if (on_done_)
      on_done_(pending_node_id_, accumulated_);
    pending_node_id_ = CANARD_NODE_ID_UNSET;
  }
}

// ---------------------------------------------------------------------------
// HandleResponse  (uavcan.register.List.1.0 response)
// ---------------------------------------------------------------------------
void SubjectScanner::HandleResponse(const CanardRxTransfer& transfer) {
  const uint8_t node_id = static_cast<uint8_t>(transfer.metadata.remote_node_id);
  if (node_id != pending_node_id_) {
    return;
  }

  // Response: variable-length string (register name), max 255 bytes
  // An empty name signals end of register list.
  const uint8_t* p = static_cast<const uint8_t*>(transfer.payload);
  if (transfer.payload_size < 1) {
    goto done;
  }

  {
    const uint8_t name_len = *p++;
    if (name_len == 0) {
      goto done;  // end of list
    }
    if (static_cast<size_t>(name_len) > transfer.payload_size - 1) {
      goto done;
    }
    std::string reg_name(reinterpret_cast<const char*>(p), name_len);

    // Filter: keep only "uavcan.pub.<name>.id" registers
    if (reg_name.starts_with(kPubPrefix)) {
      SubjectInfo si{};
      // Extract the subject name between "uavcan.pub." and ".id"
      const size_t start = kPubPrefix.size();
      const size_t end_pos = reg_name.rfind(".id");
      if (end_pos != std::string::npos && end_pos > start) {
        si.data_type = reg_name.substr(start, end_pos - start);
      } else {
        si.data_type = reg_name;
      }
      si.subject_id = 0xFFFFu;  // will be filled in when register.Access resolves the value
      accumulated_.push_back(si);
    }

    ++next_index_;
    RequestNextPage();
    return;
  }

done:
  CYMON_LOGI(kTag, "Subject scan done for node %u: %zu subjects", node_id, accumulated_.size());
  if (on_done_) {
    on_done_(node_id, accumulated_);
  }
  pending_node_id_ = CANARD_NODE_ID_UNSET;
}

}  // namespace cymon
