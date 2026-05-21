#pragma once

#include <cstdint>
#include <functional>

#include "monitor/session_manager.hpp"

namespace cymon {

class WebServer;

/// Serialises all active session graph data to MessagePack binary and
/// broadcasts via WebSocket at kPublishHz.
///
/// Wire format (MessagePack map, server → client, one message per session):
///
///   fixmap(3)
///     "s"  → uint8  session_id     (positive fixint 0x00–0x7F, or 0xCC+byte)
///     "n"  → uint8  num_channels   (positive fixint 0x00–0x7F, or 0xCC+byte)
///     "ch" → fixarray / array16 of per-channel fixmap(4):
///              "nid" → uint8  node_id      (positive fixint / 0xCC+byte)
///              "vid" → uint8  variable_id  (positive fixint / 0xCC+byte)
///              "t"   → fixarray / array16 of uint64 (0xCF + 8 bytes big-endian)
///              "v"   → fixarray / array16 of float32 (0xCA + 4 bytes IEEE-754 BE)
///
/// Encoding is produced by msgpack::BufWriter (ws_streamer.cpp).  If the
/// internal scratch buffer overflows, the message is silently dropped and a
/// warning is logged via CYMON_LOGW.
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
