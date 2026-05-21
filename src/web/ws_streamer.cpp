#include "web/ws_streamer.hpp"

#include <cassert>
#include <string_view>

#include "logging/logger.hpp"
#include "web/web_server.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.WS";

// ---------------------------------------------------------------------------
// Bounds-safe MessagePack encoder
//
// BufWriter tracks a write pointer and an end pointer.  Every Put() call
// checks for overflow before writing; once overflow is detected the writer
// enters a failed state and all subsequent writes are no-ops.  Call ok()
// and size() after all writes to retrieve the result.
// ---------------------------------------------------------------------------
namespace msgpack {

class BufWriter {
 public:
  BufWriter(uint8_t* buf, size_t cap) noexcept : begin_(buf), p_(buf), end_(buf + cap) {
    // buf must be non-null; cap==0 is valid (any write will set ok_=false)
    assert(buf != nullptr);
  }

  [[nodiscard]] bool ok() const noexcept {
    return ok_;
  }
  [[nodiscard]] size_t size() const noexcept {
    return ok_ ? static_cast<size_t>(p_ - begin_) : 0u;
  }

  /// fixmap — up to 15 pairs
  void FixMap(uint8_t n) {
    Put(static_cast<uint8_t>(0x80u | (n & 0x0Fu)));
  }

  /// fixarray (n ≤ 15) or array16 (n ≤ 65535)
  void FixArray(uint16_t n) {
    if (n <= 15u) {
      Put(static_cast<uint8_t>(0x90u | (n & 0x0Fu)));
    } else {
      Put(0xDCu);
      Put(static_cast<uint8_t>((n >> 8) & 0xFFu));
      Put(static_cast<uint8_t>(n & 0xFFu));
    }
  }

  /// fixstr — fails the writer if the string length exceeds 31 bytes
  void FixStr(std::string_view s) {
    if (s.size() > 31u) {
      ok_ = false;
      assert(false && "FixStr: string length exceeds fixstr limit (31 bytes)");
      return;
    }
    Put(static_cast<uint8_t>(0xA0u | (s.size() & 0x1Fu)));
    for (char c : s)
      Put(static_cast<uint8_t>(c));
  }

  /// positive fixint (0–127) or uint8 (0xCC prefix) for larger values
  void Uint8(uint8_t v) {
    if (v <= 0x7Fu) {
      Put(v);
    } else {
      Put(0xCCu);
      Put(v);
    }
  }

  /// uint32 (always 5-byte form)
  void Uint32(uint32_t v) {
    Put(0xCEu);
    Put(static_cast<uint8_t>((v >> 24) & 0xFFu));
    Put(static_cast<uint8_t>((v >> 16) & 0xFFu));
    Put(static_cast<uint8_t>((v >> 8) & 0xFFu));
    Put(static_cast<uint8_t>(v & 0xFFu));
  }

  /// uint64 (always 9-byte form)
  void Uint64(uint64_t v) {
    Put(0xCFu);
    for (int i = 7; i >= 0; --i)
      Put(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  /// float32 (always 5-byte form, IEEE-754 big-endian)
  void Float32(float v) {
    Put(0xCAu);
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v));
    __builtin_memcpy(&bits, &v, sizeof(bits));
    Put(static_cast<uint8_t>((bits >> 24) & 0xFFu));
    Put(static_cast<uint8_t>((bits >> 16) & 0xFFu));
    Put(static_cast<uint8_t>((bits >> 8) & 0xFFu));
    Put(static_cast<uint8_t>(bits & 0xFFu));
  }

 private:
  void Put(uint8_t b) noexcept {
    if (!ok_ || p_ >= end_) {
      ok_ = false;
      return;
    }
    *p_++ = b;
  }

  uint8_t* const begin_;
  uint8_t* p_;
  uint8_t* const end_;
  bool ok_{true};
};

}  // namespace msgpack

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
WsStreamer::WsStreamer(SessionManager& session_manager, WebServer& web_server)
    : session_manager_(session_manager), web_server_(web_server) {}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void WsStreamer::Tick(uint64_t now_us) {
  if (now_us - last_publish_us_ < kPublishIntervalUs) {
    return;
  }
  last_publish_us_ = now_us;

  session_manager_.ForEachSession([this](const MonitorSession& session) {
    GraphDataResponse resp{};
    session.GetGraphData(resp);
    if (resp.num_channels == 0) {
      return;
    }

    const size_t len = Serialise(resp, scratch_, sizeof(scratch_));
    if (len > 0) {
      web_server_.BroadcastWs(scratch_, len);
    }
  });
}

// ---------------------------------------------------------------------------
// Serialise  — produces MessagePack binary in @p out
// ---------------------------------------------------------------------------
size_t WsStreamer::Serialise(const GraphDataResponse& resp, uint8_t* out, size_t max_len) {
  msgpack::BufWriter w{out, max_len};

  // Top-level fixmap: {"s":…,"n":…,"ch":[…]}
  w.FixMap(3);
  w.FixStr("s");
  w.Uint8(resp.session_id);
  w.FixStr("n");
  w.Uint8(resp.num_channels);
  w.FixStr("ch");
  w.FixArray(resp.num_channels);

  for (uint8_t ch = 0; ch < resp.num_channels; ++ch) {
    const ChannelSnapshot& snap = resp.channels[ch];

    // Per-channel fixmap: {"nid":…,"vid":…,"t":[…],"v":[…]}
    w.FixMap(4);
    w.FixStr("nid");
    w.Uint8(snap.key.node_id);
    w.FixStr("vid");
    w.Uint8(snap.key.variable_id);

    // Timestamps — array of uint64
    w.FixStr("t");
    w.FixArray(static_cast<uint16_t>(snap.count));
    for (size_t i = 0; i < snap.count; ++i) {
      w.Uint64(snap.samples[i].timestamp_us);
    }

    // Values — array of float32
    w.FixStr("v");
    w.FixArray(static_cast<uint16_t>(snap.count));
    for (size_t i = 0; i < snap.count; ++i) {
      w.Float32(snap.samples[i].value);
    }
  }

  if (!w.ok()) {
    CYMON_LOGW(kTag, "WS frame truncated: scratch buffer too small");
  }
  return w.size();
}

}  // namespace cymon
