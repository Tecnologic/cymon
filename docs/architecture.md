# cymon architecture

## Decision: host application instead of embedded firmware

cymon 0.1 ran on an ESP32-S2 with an MCP2518FD and served the UI over WiFi.
With UNIMOC as the main device that design ran out of room:

- An admin tool has to decode any DSDL type at run time: standard,
  `cymon` and project namespaces such as `unimoc`. A microcontroller only
  has the types compiled in.
- UNIMOC exposes 77 registers and will add firmware updates over Cyphal
  (`uavcan.file.Read` from the tool), recordings and multi-node views of the
  cart. All of these want a file system, memory and a real UI.
- The UNIMOC simulation runs on the PC (`native_sim` on vcan or slcan). A
  host tool reaches it without hardware.
- The bus is the bottleneck, not the tool. The BCC runs 1 Mbit/s without
  bit rate switch, telemetry is 20 Hz, and captures are read page by page.

Two alternatives were considered and not chosen:

- **STM32H7 with Zephyr**: more RAM than the ESP32, but every point above
  still applies. It would also be another board to bring up.
- **Raspberry Pi**: not ruled out. The Pi runs this same package with a CAN
  FD HAT (SocketCAN) when a headless box in the vehicle is needed.

## Structure

```
browser ── HTTP/WebSocket ──> FastAPI (web/server.py)
                                  │
                                Admin (admin.py) ── one pycyphal node, node-ID 127
                                  ├─ NodeTracker, heartbeat, port.List, diagnostic.Record
                                  ├─ RegisterClient (registers.py)  uavcan.register.List/Access
                                  ├─ ScopeClient   (scope.py)       cymon.* services
                                  ├─ SubjectMonitor / PublisherManager (subjects.py)
                                  └─ CentralizedAllocator (optional PnP)
                                  │
                         pycyphal transport (transport.py)
                           CAN: canable (canable.py) | socketcan | python-can | redundant group
                           UDP: simulation
```

- Everything runs in one asyncio loop: the pycyphal node, the web server
  and the publications.
- DSDL is compiled at start-up by `dsdl.setup()`: `uavcan`, `reg`, `cymon`
  and the `--dsdl` roots. The cache is keyed by the content of the DSDL
  and the pycyphal version.
- The UI is static HTML/JS (Alpine.js and uPlot, vendored). There is no
  Node.js build step, and it works offline.
- Events (node changes, diagnostics, subject batches every 50 ms, capture
  progress) go to every WebSocket. A slow client drops its own oldest
  events and never blocks the bus.

### cymon services discovery

The cymon services have no fixed port-IDs. The tool:

1. reads the register list of the node;
2. reads every `uavcan.srv.*.type` register;
3. uses the ID register of each one whose type is `cymon.<Service>.0.1`.

The same mechanism maps publishers and subscribers to their types, which
gives the **monitor** and **publish** buttons in the port table.

### Safety model

- Publications have a lease that only the owning browser tab renews.
  A WebSocket disconnect stops them immediately; a hung tab stops them
  after the lease.
- E-STOP stops everything the tool publishes and sends EMERGENCY_STOP to
  every online node in parallel, with a 0.5 s timeout each.
- The web server binds to localhost unless told otherwise.

## Roadmap

| Item | Notes |
|---|---|
| Firmware update | `uavcan.file.Read` server for an image file plus `ExecuteCommand BEGIN_SOFTWARE_UPDATE`; UNIMOC plans bank-2 updates |
| Recording | Subject streams and captures to disk (CSV or MCAP), replay in the monitor |
| Multi-bus | Several transports in one instance, one per bus (today: one instance per bus) |
| Cart view | Dashboard of the four drives and the VDC (UNIMOC `docs/system_plan.md`) |
| Trigger fan-out | Publish `cymon.TriggerEvent` from the tool to start captures on several nodes at once |
| Register presets | Save and load register sets per node type |
