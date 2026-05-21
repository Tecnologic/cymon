#pragma once

#include <cstdint>
#include <functional>

#include "monitor/session_manager.hpp"

namespace cymon {

class WebServer;

/// Serialises all active session graph data to MessagePack binary and
/// broadcasts via WebSocket at kPublishHz.
///
/// MessagePack format (server → client, per session):
/// {
///   "s": uint8          -- session_id
///   "n": uint8          -- num_channels
///   "ch": [             -- array of channels
///     {
///       "nid": uint8,
///       "vid": uint8,
///       "t":  [uint64, ...]   -- timestamp_us array
///       "v":  [float32, ...]  -- value array (MessagePack float32 / 0xCA)
///     }, ...
///   ]
/// }
class WsStreamer {
 public:
  static constexpr uint32_t kPublishHz = 10;
  static constexpr uint64_t kPublishIntervalUs = 1000000u / kPublishHz;

  WsStreamer(SessionManager& session_manager, WebServer& web_server);

  /// Call from ws_streamer_task at kPublishHz.
  void Tick(uint64_t now_us);

 private:
  size_t Serialise(const GraphDataResponse& resp, uint8_t* out, size_t max_len);

  SessionManager& session_manager_;
  WebServer& web_server_;
  uint64_t last_publish_us_{0};

  // Static scratch buffer — lives in DRAM, shared across calls (single writer)
  static constexpr size_t kMaxFrameBytes = 16384;
  uint8_t scratch_[kMaxFrameBytes]{};
};

}  // namespace cymon
