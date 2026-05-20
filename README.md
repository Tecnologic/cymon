# cymon

**Cyphal network monitor firmware for ESP32-S2**

cymon is an ESP32-S2 firmware that acts as a Cyphal (formerly UAVCAN) network
monitor with a browser-based oscilloscope UI.  It speaks CAN-FD via an
MCP2518FD controller and exposes a WebSocket/REST API served over WiFi.

---

## Architecture

```
cymon-lib (git submodule — device-node only)
  └── cymon::Device — variable registry, trigger, Cyphal service handlers
      (consumed by device nodes, NOT by this firmware)

cymon (this repo — monitor)
  ├── CAN-FD driver    src/can/mcp2518fd.*
  ├── Cyphal transport src/can/cyphal_transport.*   (libcanard + o1heap)
  ├── Cyphal node      src/cyphal/node.*            (heartbeat, GetInfo)
  ├── Network scanner  src/cyphal/scanner.*         (heartbeat → NodeRecord)
  ├── Variable fetcher src/cyphal/variable_fetcher.*
  ├── Subject scanner  src/cyphal/subject_scanner.*
  ├── PnP allocator    src/cyphal/allocator.*
  ├── Time sync        src/cyphal/timesync.*
  ├── Monitor engine   src/monitor/                 (ring buffers, sessions)
  ├── WebSocket stream src/web/ws_streamer.*        (MessagePack binary)
  ├── REST API         src/web/api_handlers.*
  ├── HTTP server      src/web/web_server.*
  ├── WiFi manager     src/web/wifi_manager.*
  └── Web UI           data/www/                    (Alpine + Chart.js SPA)
```

---

## Hardware

| Signal | ESP32-S2 GPIO |
|--------|---------------|
| SPI MOSI | 11 |
| SPI MISO | 13 |
| SPI SCK  | 12 |
| SPI CS   | 10 |
| MCP2518FD INT | 9 |

Adjust `kCanPins` in `src/main.cpp` to match your PCB.

---

## Building

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)
- clang-18, cmake ≥ 3.20 (for host tests)

### Firmware (ESP32-S2)

```bash
# Init submodules
git submodule update --init --recursive

# Download vendor JS for SPIFFS (once)
curl -Lo data/www/vendor/chart.umd.min.js \
  https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js
curl -Lo data/www/vendor/msgpack.min.js \
  https://cdn.jsdelivr.net/npm/@msgpack/msgpack@3.0.0/dist.es5+umd/msgpack.min.js
curl -Lo data/www/vendor/alpine.min.js \
  https://cdn.jsdelivr.net/npm/alpinejs@3.14.1/dist/cdn.min.js

# Build firmware
pio run --environment esp32s2mini

# Flash firmware + filesystem
pio run --environment esp32s2mini --target upload
pio run --environment esp32s2mini --target uploadfs
```

### Host tests (Linux, no hardware)

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## WebSocket Protocol

Binary MessagePack, endpoint `ws://<device-ip>/ws`, 10 Hz.

```
{
  "s": <session_id: uint8>,
  "n": <num_channels: uint8>,
  "ch": [
    { "nid": uint8, "vid": uint8,
      "t": [uint64, …],   // microsecond timestamps
      "v": [float32, …]   // sample values — NO interpolation
    }, …
  ]
}
```

---

## REST API

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/nodes` | Node table (JSON) |
| POST | `/api/session` | Create monitor session |
| DELETE | `/api/session/:id` | Delete session |
| POST | `/api/wifi` | Update WiFi credentials |
| POST | `/api/can` | Update CAN baud rates |
| GET | `/api/settings` | Read current settings |

---

## License

Apache 2.0 — see [LICENSE](LICENSE).
