#include "web/ws_streamer.hpp"

#include "logging/logger.hpp"
#include "web/web_server.hpp"

namespace cymon {

static constexpr const char* kTag = "CYMON.WS";

// ---------------------------------------------------------------------------
// Minimal MessagePack encoder (subset: fixmap, fixarray, fixstr, uint8,
// uint16, uint32, uint64, float32, bin8, bin16)
// ---------------------------------------------------------------------------
namespace msgpack {

static uint8_t* WriteUint8(uint8_t* p, uint8_t v) {
  if (v <= 0x7Fu) {
    *p++ = v;
  } else {
    *p++ = 0xCCu;
    *p++ = v;
  }
  return p;
}

static uint8_t* WriteUint32(uint8_t* p, uint32_t v) {
  *p++ = 0xCEu;
  *p++ = static_cast<uint8_t>((v >> 24) & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 16) & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 8) & 0xFFu);
  *p++ = static_cast<uint8_t>(v & 0xFFu);
  return p;
}

static uint8_t* WriteUint64(uint8_t* p, uint64_t v) {
  *p++ = 0xCFu;
  for (int i = 7; i >= 0; --i) {
    *p++ = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
  }
  return p;
}

static uint8_t* WriteFloat32(uint8_t* p, float v) {
  *p++ = 0xCAu;
  uint32_t bits;
  static_assert(sizeof(bits) == sizeof(v));
  __builtin_memcpy(&bits, &v, 4);
  *p++ = static_cast<uint8_t>((bits >> 24) & 0xFFu);
  *p++ = static_cast<uint8_t>((bits >> 16) & 0xFFu);
  *p++ = static_cast<uint8_t>((bits >> 8) & 0xFFu);
  *p++ = static_cast<uint8_t>(bits & 0xFFu);
  return p;
}

static uint8_t* WriteFixStr(uint8_t* p, const char* s, uint8_t len) {
  *p++ = static_cast<uint8_t>(0xA0u | (len & 0x1Fu));
  for (uint8_t i = 0; i < len; ++i)
    *p++ = static_cast<uint8_t>(s[i]);
  return p;
}

static uint8_t* WriteFixMap(uint8_t* p, uint8_t n) {
  *p++ = static_cast<uint8_t>(0x80u | (n & 0x0Fu));
  return p;
}

static uint8_t* WriteFixArray(uint8_t* p, uint16_t n) {
  if (n <= 15) {
    *p++ = static_cast<uint8_t>(0x90u | (n & 0x0Fu));
  } else {
    *p++ = 0xDCu;
    *p++ = static_cast<uint8_t>((n >> 8) & 0xFFu);
    *p++ = static_cast<uint8_t>(n & 0xFFu);
  }
  return p;
}

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
  uint8_t* p = out;
  uint8_t* const end = out + max_len;

#define CHECK(n)       \
  if ((p + (n)) > end) \
  return 0

  // Top-level fixmap: {"s":…,"n":…,"ch":[…]}
  CHECK(1);
  p = msgpack::WriteFixMap(p, 3);

  CHECK(2);
  p = msgpack::WriteFixStr(p, "s", 1);
  CHECK(2);
  p = msgpack::WriteUint8(p, resp.session_id);

  CHECK(2);
  p = msgpack::WriteFixStr(p, "n", 1);
  CHECK(2);
  p = msgpack::WriteUint8(p, resp.num_channels);

  CHECK(3);
  p = msgpack::WriteFixStr(p, "ch", 2);
  CHECK(3);
  p = msgpack::WriteFixArray(p, resp.num_channels);

  for (uint8_t ch = 0; ch < resp.num_channels; ++ch) {
    const ChannelSnapshot& snap = resp.channels[ch];

    // Per-channel fixmap: {"nid":…,"vid":…,"t":[…],"v":[…]}
    CHECK(1);
    p = msgpack::WriteFixMap(p, 4);

    CHECK(5);
    p = msgpack::WriteFixStr(p, "nid", 3);
    p = msgpack::WriteUint8(p, snap.key.node_id);

    CHECK(5);
    p = msgpack::WriteFixStr(p, "vid", 3);
    p = msgpack::WriteUint8(p, snap.key.variable_id);

    // Timestamps
    CHECK(2);
    p = msgpack::WriteFixStr(p, "t", 1);
    CHECK(3);
    p = msgpack::WriteFixArray(p, static_cast<uint16_t>(snap.count));
    for (size_t i = 0; i < snap.count; ++i) {
      CHECK(9);
      p = msgpack::WriteUint64(p, snap.samples[i].timestamp_us);
    }

    // Values — packed as array of float32
    CHECK(2);
    p = msgpack::WriteFixStr(p, "v", 1);
    CHECK(3);
    p = msgpack::WriteFixArray(p, static_cast<uint16_t>(snap.count));
    for (size_t i = 0; i < snap.count; ++i) {
      CHECK(5);
      p = msgpack::WriteFloat32(p, snap.samples[i].value);
    }
  }

#undef CHECK

  return static_cast<size_t>(p - out);
}

}  // namespace cymon
