# cymon

[![CI](https://github.com/Tecnologic/cymon/actions/workflows/ci.yml/badge.svg)](https://github.com/Tecnologic/cymon/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Tecnologic/cymon)](https://github.com/Tecnologic/cymon/releases/latest)

**Cyphal admin tool for the host.** cymon connects to a Cyphal/CAN bus
through one or more USB CAN FD adapters (CANable 2.0 / 2.0 Pro and others)
and serves a web UI on the same computer:

- **Bus**: load your DSDL namespaces, pick the CAN adapter (detected ones
  are listed), bit rates and node-ID, connect and disconnect, all without
  restarting. Or start the simulated devices.
- **Nodes**: every node on the bus with health, mode, uptime and vendor
  status code. The detail view shows GetInfo (name, versions, VCS revision,
  unique-ID) and the port list.
- **Registers**: list, read and write all registers of a node. Values are
  type- and range-checked before a write. The node's port registers
  (`uavcan.pub/sub/srv.*.id/type`) map every port to its DSDL type.
- **Commands**: restart, store persistent states, factory reset,
  emergency stop, or any command code.
- **Monitor**: subscribe to any subject with any DSDL type (standard types,
  `cymon`, your own namespaces such as `unimoc`) and plot its numeric fields
  live.
- **Publish**: periodic publications, for example
  `reg.udral.service.common.Readiness` plus a setpoint, with a dead-man
  lease. They stop when the browser tab closes or loses its connection.
- **Scope**: oscilloscope-style captures from nodes running
  [cymon-lib](https://github.com/Tecnologic/cymon-lib): variable list,
  trigger (rising, falling, hysteresis, immediate), pre-trigger, decimation,
  plot and CSV export.
- **Log**: the `uavcan.diagnostic.Record` messages of all nodes.
- **Theme**: dark by default; the ☀/☾ button in the header switches to light (remembered per browser).
- **E-STOP**: stops all publications and sends EMERGENCY_STOP to every
  online node.
- **Plug-and-play**: an optional node-ID allocator (`--pnp`).

cymon started as ESP32-S2 firmware with a WiFi web UI. That chip was too
small for the register browser, dynamic DSDL types and captures that
[UNIMOC](https://github.com/Tecnologic/unimoc) needs, so cymon runs on the
host now (0.1.0 is the first release of the host tool) and the firmware is gone. See [docs/architecture.md](docs/architecture.md).

## Quick start: executable

Download `cymon-<version>-windows-x86_64.exe` or `cymon-<version>-linux-x86_64`
from the [latest release](https://github.com/Tecnologic/cymon/releases/latest)
and start it (Linux: `chmod +x` first). Without parameters, cymon opens the
web UI in the browser on the **Bus** tab:

1. **DSDL**: enter the root namespace directory of your types, e.g.
   `C:\work\unimoc\dsdl\unimoc`, and press **Load**. The standard types and
   `cymon` are built in.
2. **Interface**: **search adapters** lists the CANable 2.0 serial ports
   and SocketCAN interfaces; **use** one, or type it (`canable:COM5`, see
   the table below). Choose **Demo** to try cymon without hardware.
3. **Connect**.

The browser remembers the form and the DSDL list for the next start. The
console window shows the log; close it or press Ctrl+C to quit. The first
start compiles the DSDL, which takes a few seconds and is cached in
`~/.cache/cymon`. All command line options below work with the executable
too (`cymon.exe serve -i canable:COM5`).

## Quick start: Python package (no hardware)

```sh
git clone --recurse-submodules https://github.com/Tecnologic/cymon.git
cd cymon
python -m venv .venv && . .venv/bin/activate      # Windows: .venv\Scripts\activate
pip install -e .
cymon demo --open
```

`cymon demo` runs simulated devices (node-IDs 10, 11, ...) on an in-process
virtual CAN bus. Each one has registers, telemetry, a readiness input that
times out, and the cymon services. The first start compiles the DSDL, which
takes a few seconds and is cached in `~/.cache/cymon`.

## Connecting to a bus

```sh
cymon adapters                                   # what is connected
cymon serve -i canable:/dev/ttyACM0 --open       # Linux/macOS; Windows: -i canable:COM5
cymon                                            # no interface: choose it in the web UI (Bus tab)
```

The web UI listens on <http://localhost:8765>. It binds to `127.0.0.1` by
default. With `--host 0.0.0.0` anyone on your network can command the
bus.

| Option | Default | Meaning |
|---|---|---|
| `-i KIND:CHANNEL` | — | Interface; repeat it for a redundant bus (see below). Without it, connect from the web UI |
| `--bitrate` | 1000000 | Nominal (arbitration) bit rate |
| `--data-bitrate` | none | CAN FD data rate. Without it, FD frames are sent **without bit rate switch** at the nominal rate |
| `--classic` | off | Classic CAN (MTU 8) instead of CAN FD (MTU 64) |
| `--node-id` | 127 | Node-ID of the tool |
| `--dsdl DIR` | — | Extra DSDL root namespace (repeatable, also `$CYMON_DSDL`) |
| `--pnp [DB]` | off | Run the plug-and-play allocator with this database (`cymon-pnp.db`) |
| `--host`, `--port` | 127.0.0.1, 8765 | Address of the web UI |

Interfaces:

| `-i` | Adapter |
|---|---|
| `canable:/dev/ttyACM0`, `canable:COM5` | CANable 2.0 / 2.0 Pro with the **slcan** firmware (CAN FD), any OS |
| `socketcan:can0` | Linux SocketCAN: CANable with the **candleLight** firmware, `vcan0`, any other SocketCAN device |
| `pcan:PCAN_USBBUS1`, `gs_usb:0`, `kvaser:0`, ... | Other [python-can interfaces](https://python-can.readthedocs.io/en/stable/interfaces.html) that pycyphal supports |
| `slcan:/dev/pts/3` | Classic CAN slcan, e.g. the pseudo terminals of the UNIMOC simulation (with `--classic`) |
| `udp:127.0.0.1` | Cyphal/UDP (simulation) |

### CAN adapters: CANable 2.0 (Pro)

The CANable 2.0 is an STM32G431 board with CAN FD. Two firmwares exist:

- **slcan** (the CANable 2.0 default, works on Windows, Linux and macOS)
  appears as a serial port (USB 16d0:117e). Use `-i canable:<port>`. The
  firmware can send FD frames without bit rate switch at the nominal rate
  (the default here), or with bit rate switch at 2 or 5 Mbit/s
  (`--data-bitrate 2000000` or `5000000`). Its nominal rates are 10k, 20k,
  50k, 83.3k, 100k, 125k, 250k, 500k, 750k and 1M.
- **candleLight** (gs_usb, Linux) is a kernel SocketCAN interface with hardware
  timestamps and any bit timing. Set it up with
  `sudo scripts/socketcan-up.sh` (1 Mbit/s, CAN FD; `BITRATE=...
  DBITRATE=...` for others), then use `-i socketcan:can0`.

On a Linux host that stays connected to the bus, candleLight is the better
choice. On Windows, use slcan.

**UNIMOC BCC:** its SN65HVD230 is a classic CAN transceiver. The drive
therefore sends CAN FD frames at 1 Mbit/s without bit rate switch, and the
cymon defaults (`--bitrate 1000000`, no `--data-bitrate`) match it.

**Several adapters:**

- **On one bus**, repeating `-i` makes a redundant Cyphal transport: every
  transfer goes out on all adapters, and duplicates are dropped on
  reception. Example: `-i canable:/dev/ttyACM0 -i canable:/dev/ttyACM1`.
  Use this when the bus is physically redundant (UNIMOC supports redundant
  interfaces) or to test redundancy.
- **On separate buses**, run one cymon per bus, each with its own `--port`
  and `--node-id`.

### With UNIMOC

```sh
# Simulation on vcan0 (Linux; once: sudo VCAN=1 scripts/socketcan-up.sh)
cymon serve -i socketcan:vcan0 --dsdl ../unimoc/dsdl/unimoc --open
# Simulation over its slcan pseudo terminal (WSL2 without vcan)
cymon serve -i slcan:/dev/pts/3 --classic --dsdl ../unimoc/dsdl/unimoc
# BCC on the bench, with a CANable 2.0
cymon serve -i canable:/dev/ttyACM0 --dsdl ../unimoc/dsdl/unimoc --pnp
```

With `--dsdl`, the UNIMOC types decode in the monitor (for example
`unimoc.drive.Telemetry.0.1` on subject 212). In the node's port table,
**publish** opens a prepared readiness publication and **monitor** opens a
live plot.

## Safety

cymon commands real power electronics, so it is built to fail safe:

- Every publication has a **lease** (default 1 s). The browser tab renews it
  four times a second. If the tab closes, reloads, hangs or loses the
  connection, the publications stop. A drive that needs a periodic
  readiness, such as UNIMOC with its 0.5 s timeout, then disengages by
  itself.
- **E-STOP** in the header stops all publications and sends EMERGENCY_STOP
  (65531) to every online node, without asking for confirmation.
- Register writes are converted to the register's type and range-checked.
  After the write, the UI shows the value the node actually kept.

## REST and WebSocket API

The UI uses only this API, so scripts can use it as well. OpenAPI
documentation is at `/docs`. Without a bus connection, the endpoints that
need one answer 503.

| Method | Path | |
|---|---|---|
| GET | `/api/status`, `/api/adapters` | Tool and connection, CAN adapters found on this computer |
| POST | `/api/connect`, `/api/disconnect` | Connect (`{"interfaces": [...], "bitrate", "data_bitrate", "classic", "node_id", "pnp", "demo_devices"}`), disconnect |
| GET / PUT | `/api/dsdl` | Extra DSDL root namespaces (`{"roots": [...]}`; PUT compiles and loads them) |
| GET | `/api/nodes`, `/api/diagnostics`, `/api/types` | Node table, log, known message types |
| GET | `/api/types/{type}` | Default value of a type, as a publish template |
| GET / PUT | `/api/nodes/{id}/registers[/{name}]` | Read all, read one, write one (`{"value": ...}`) |
| GET | `/api/nodes/{id}/ports` | Port registers: direction, name, ID, type |
| POST | `/api/nodes/{id}/command` | `{"command": "restart" \| 65530 \| ..., "parameter": ""}` |
| POST | `/api/estop` | E-STOP |
| GET / POST / DELETE | `/api/subjects[/{id}]` | Monitor subscriptions (`{"subject_id", "type"}`) |
| GET / POST / PUT / DELETE | `/api/publishers[/{id}]`, POST `/api/publishers/stop` | Publications (`{"subject_id", "type", "value", "rate_hz", "lease_s"}`) |
| GET | `/api/nodes/{id}/scope[/status]` | cymon variables and buffer, capture state |
| POST | `/api/nodes/{id}/scope/capture`, `/read`, `/disarm` | Run a capture (blocks until complete), read it again, disarm |
| WS | `/ws?client=<id>` | All events as JSON; send `{"type": "renew"}` to keep the publications of `<id>` alive |

A publication belongs to the client named in the `X-Cymon-Client` header
of its POST. It stops when that client's WebSocket closes.

## Development

```sh
pip install -e '.[dev]'
ruff check src tests packaging && ruff format --check src tests packaging
pytest -q                      # admin node against the simulated device on a virtual CAN bus

pip install pyinstaller        # the executable, as CI builds it
pyinstaller --noconfirm packaging/cymon.spec && python packaging/smoke_test.py dist/cymon
```

| Path | Content |
|---|---|
| `src/cymon_app/` | The package. The DSDL namespace `cymon` becomes a Python package of that name, hence `cymon_app` |
| `dsdl.py` | DSDL compilation (cached) and type lookup |
| `canable.py`, `transport.py` | CANable 2.0 slcan FD bus (python-can plugin `canable`), transports from `-i` |
| `session.py` | The bus connection: connect and disconnect at run time, DSDL loading |
| `admin.py` | The tool's node: node table, port lists, diagnostics, commands, PnP |
| `registers.py`, `subjects.py`, `scope.py` | Register client, monitor/publisher, cymon client |
| `demo.py` | Simulated device, used by `cymon demo` and the tests |
| `web/` | FastAPI server and the static UI (Alpine.js and uPlot, vendored; no build step) |
| `lib/cymon-lib` | Submodule: the device library and the `cymon` DSDL |
| `dsdl/public_regulated_data_types` | Submodule: the standard `uavcan` and `reg` namespaces, at the same commit as UNIMOC |
| `packaging/` | PyInstaller spec of the executable and its smoke test |

### Releases

Versions follow [semantic versioning](https://semver.org) and are made by
[release-please](https://github.com/googleapis/release-please) from
[conventional commits](https://www.conventionalcommits.org) on `main`
(squash merges: the pull request title is the commit):

| Commit | Example | Version (below 1.0) |
|---|---|---|
| `fix: ...` | `fix: CANable data bit rate on Windows` | 0.1.0 → 0.1.1 |
| `feat: ...` | `feat: register presets` | 0.1.0 → 0.2.0 |
| `feat!: ...` or `BREAKING CHANGE:` | `feat!: rename /api/subjects` | 0.1.0 → 0.2.0 |
| `docs:`, `test:`, `ci:`, `chore:`, `refactor:` | | none |

Every push to `main` updates the release pull request, which bumps the
version in `pyproject.toml` and `cymon_app/__init__.py` and adds to
`CHANGELOG.md`. Merging it tags `vX.Y.Z`, creates the GitHub release and
attaches the Linux and Windows executables and the wheel. Pull requests
build the executables too (artifacts of the CI run).

## License

AGPL-3.0-or-later; see [LICENSE](LICENSE). cymon-lib, which goes into
firmware, is Apache-2.0.
