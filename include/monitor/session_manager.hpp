#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "monitor/monitor_session.hpp"

namespace cymon {

/// Maximum number of concurrent monitor sessions.
inline constexpr size_t kMaxConcurrentSessions = 4;

/// Manages a pool of MonitorSession objects and routes incoming samples.
///
/// Thread-safety:
///   FeedSample() may be called from any task.  CreateSession() /
///   DestroySession() must not be called concurrently with FeedSample().
///   On the target the caller must take the session_mutex before modifying
///   the session table.  In host tests no concurrency is exercised.
class SessionManager {
 public:
  SessionManager() = default;

  /// Create a new session.  Returns session_id on success, 0xFF on failure
  /// (all slots occupied).
  [[nodiscard]] uint8_t CreateSession(std::span<const ChannelConfig> channels, SessionMode mode,
                                      const TriggerConfig* trigger = nullptr) noexcept;

  /// Destroy a session by id.  Returns true if found and removed.
  bool DestroySession(uint8_t session_id) noexcept;

  /// Route a sample to all sessions that subscribe to (node_id, variable_id).
  void FeedSample(uint8_t node_id, uint8_t variable_id, uint64_t timestamp_us, float value) noexcept;

  /// Retrieve graph data from a specific session.
  /// Returns false if session_id not found.
  bool GetGraphData(uint8_t session_id, GraphDataResponse& response) const noexcept;

  /// Call @p fn for each active session (read-only).
  void ForEachSession(const std::function<void(const MonitorSession&)>& fn) const noexcept;

  [[nodiscard]] size_t SessionCount() const noexcept;

 private:
  static constexpr uint8_t kInvalidId = 0xFF;

  std::array<std::unique_ptr<MonitorSession>, kMaxConcurrentSessions> sessions_{};
  uint8_t next_id_{1};
};

}  // namespace cymon
