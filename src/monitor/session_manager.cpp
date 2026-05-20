#include "monitor/session_manager.hpp"

#include <algorithm>

namespace cymon {

// ---------------------------------------------------------------------------
// CreateSession
// ---------------------------------------------------------------------------
uint8_t SessionManager::CreateSession(std::span<const ChannelConfig> channels, SessionMode mode,
                                      const TriggerConfig* trigger) noexcept {
  for (size_t i = 0; i < kMaxConcurrentSessions; ++i) {
    if (!sessions_[i]) {
      const uint8_t id = next_id_++;
      if (next_id_ == kInvalidId) {
        next_id_ = 1;
      }
      sessions_[i] = std::make_unique<MonitorSession>(id);
      sessions_[i]->ConfigureChannels(channels);
      sessions_[i]->SetMode(mode);
      if (trigger != nullptr) {
        sessions_[i]->ConfigureTrigger(*trigger);
      }
      return id;
    }
  }
  return kInvalidId;
}

// ---------------------------------------------------------------------------
// DestroySession
// ---------------------------------------------------------------------------
bool SessionManager::DestroySession(uint8_t session_id) noexcept {
  for (size_t i = 0; i < kMaxConcurrentSessions; ++i) {
    if (sessions_[i] && sessions_[i]->Id() == session_id) {
      sessions_[i].reset();
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// FeedSample
// ---------------------------------------------------------------------------
void SessionManager::FeedSample(uint8_t node_id, uint8_t variable_id, uint64_t timestamp_us,
                                float value) noexcept {
  for (auto& session : sessions_) {
    if (session) {
      session->FeedSample(node_id, variable_id, timestamp_us, value);
    }
  }
}

// ---------------------------------------------------------------------------
// GetGraphData
// ---------------------------------------------------------------------------
bool SessionManager::GetGraphData(uint8_t session_id, GraphDataResponse& response) const noexcept {
  for (const auto& session : sessions_) {
    if (session && session->Id() == session_id) {
      session->GetGraphData(response);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// ForEachSession
// ---------------------------------------------------------------------------
void SessionManager::ForEachSession(const std::function<void(const MonitorSession&)>& fn) const noexcept {
  for (const auto& session : sessions_) {
    if (session) {
      fn(*session);
    }
  }
}

// ---------------------------------------------------------------------------
// SessionCount
// ---------------------------------------------------------------------------
size_t SessionManager::SessionCount() const noexcept {
  size_t n = 0;
  for (const auto& session : sessions_) {
    if (session) {
      ++n;
    }
  }
  return n;
}

}  // namespace cymon
