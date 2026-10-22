# SPDX-License-Identifier: AGPL-3.0-or-later
"""The admin node: one Cyphal node on the bus with everything the web UI uses.

Import only after :func:`cymon_app.dsdl.setup`.
"""

from __future__ import annotations

import asyncio
import collections
import dataclasses
import logging
import socket
import time
import uuid
from pathlib import Path
from typing import Any

import pycyphal.application
import pycyphal.dsdl
import pycyphal.transport
import uavcan.diagnostic
import uavcan.node
import uavcan.node.port
from pycyphal.application.node_tracker import Entry, NodeTracker

from . import __version__
from .events import EventHub
from .registers import RegisterClient
from .scope import ScopeClient
from .subjects import PublisherManager, SubjectMonitor

_logger = logging.getLogger(__name__)

HEALTH = {0: "nominal", 1: "advisory", 2: "caution", 3: "warning"}
MODE = {0: "operational", 1: "initialization", 2: "maintenance", 3: "software_update"}
SEVERITY = {0: "trace", 1: "debug", 2: "info", 3: "notice", 4: "warning", 5: "error", 6: "critical", 7: "alert"}

#: Standard commands of uavcan.node.ExecuteCommand.1.x.
COMMANDS = {
    "restart": 65535,
    "power_off": 65534,
    "begin_software_update": 65533,
    "factory_reset": 65532,
    "emergency_stop": 65531,
    "store_persistent_states": 65530,
}
COMMAND_STATUS = {
    0: "success",
    1: "failure",
    2: "not authorized",
    3: "bad command",
    4: "bad parameter",
    5: "bad state",
    6: "internal error",
}


def _ids_from_list(value: Any, limit: int) -> list[int]:
    """Port-IDs of a uavcan.node.port.SubjectIDList/ServiceIDList (mask, sparse list or total)."""
    if value is None:
        return []
    if getattr(value, "mask", None) is not None:
        return [i for i, bit in enumerate(value.mask) if bit]
    if getattr(value, "sparse_list", None) is not None:
        return sorted(int(s.value) for s in value.sparse_list)
    if getattr(value, "total", None) is not None:
        return list(range(limit + 1))
    return [i for i, bit in enumerate(value) if bit]  # ServiceIDList is a plain bit mask field


@dataclasses.dataclass
class NodeRecord:
    node_id: int
    health: str = "nominal"
    mode: str = "operational"
    uptime: int = 0
    vssc: int = 0
    online: bool = True
    last_seen: float = 0.0
    info: dict[str, Any] | None = None
    ports: dict[str, list[int]] | None = None

    def to_json(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def default_unique_id() -> bytes:
    """Stable per machine, so a PnP allocator and the node table keep recognizing this tool."""
    return uuid.uuid5(uuid.NAMESPACE_DNS, f"cymon.{socket.gethostname()}").bytes


class Admin:
    """Owns the pycyphal node of the tool and its services."""

    def __init__(
        self,
        transport: pycyphal.transport.Transport,
        *,
        name: str = "org.tecnologic.cymon",
        pnp_database: Path | str | None = None,
        unique_id: bytes | None = None,
        events: EventHub | None = None,
    ) -> None:
        self.events = events or EventHub()
        info = uavcan.node.GetInfo_1.Response(
            protocol_version=uavcan.node.Version_1(1, 0),
            software_version=uavcan.node.Version_1(*map(int, __version__.split(".")[:2])),
            unique_id=unique_id or default_unique_id(),
            name=name,
        )
        registry = pycyphal.application.make_registry(None, {})
        self.node = pycyphal.application.make_node(info, registry, transport=transport)
        self.node.heartbeat_publisher.mode = uavcan.node.Mode_1.OPERATIONAL
        self.registers = RegisterClient(self.node)
        self.scope = ScopeClient(self.node, self.registers, self.events)
        self.subjects = SubjectMonitor(self.node, self.events)
        self.publishers = PublisherManager(self.node, self.events)
        self.nodes: dict[int, NodeRecord] = {}
        self.diagnostics: collections.deque[dict[str, Any]] = collections.deque(maxlen=2000)
        self._command_clients: dict[int, Any] = {}

        self._tracker = NodeTracker(self.node)
        self._tracker.add_update_handler(self._on_tracker_update)
        self._heartbeat_sub = self.node.make_subscriber(uavcan.node.Heartbeat_1)
        self._port_list_sub = self.node.make_subscriber(uavcan.node.port.List_0_1)
        self._diagnostic_sub = self.node.make_subscriber(uavcan.diagnostic.Record_1)
        self._allocator: Any = None
        if pnp_database is not None:
            from pycyphal.application.plug_and_play import CentralizedAllocator

            self._allocator = CentralizedAllocator(self.node, pnp_database)

    @property
    def node_id(self) -> int | None:
        return self.node.id

    @property
    def pnp_enabled(self) -> bool:
        return self._allocator is not None

    def start(self) -> None:
        self.node.start()
        self._heartbeat_sub.receive_in_background(self._on_heartbeat)
        self._port_list_sub.receive_in_background(self._on_port_list)
        self._diagnostic_sub.receive_in_background(self._on_diagnostic)

    def close(self) -> None:
        self.publishers.stop_all(reason="shutdown")
        self.subjects.close()
        self.node.close()

    # --- Node table -----------------------------------------------------------------------------------------------

    def _record(self, node_id: int) -> NodeRecord:
        if node_id not in self.nodes:
            self.nodes[node_id] = NodeRecord(node_id)
        return self.nodes[node_id]

    async def _on_heartbeat(self, msg: uavcan.node.Heartbeat_1, transfer: pycyphal.transport.TransferFrom) -> None:
        if transfer.source_node_id is None:
            return
        rec = self._record(transfer.source_node_id)
        changed = (
            not rec.online
            or rec.health != HEALTH.get(msg.health.value)
            or rec.mode != MODE.get(msg.mode.value)
            or rec.vssc != msg.vendor_specific_status_code
        )
        rec.health = HEALTH.get(msg.health.value, str(msg.health.value))
        rec.mode = MODE.get(msg.mode.value, str(msg.mode.value))
        rec.uptime = int(msg.uptime)
        rec.vssc = int(msg.vendor_specific_status_code)
        rec.online = True
        rec.last_seen = time.time()
        self.events.emit("heartbeat", node_id=rec.node_id, uptime=rec.uptime)
        if changed:
            self.events.emit("node", node=rec.to_json())

    def _on_tracker_update(self, node_id: int, old: Entry | None, new: Entry | None) -> None:
        rec = self._record(node_id)
        if new is None:
            rec.online = False
            _logger.info("Node %d went offline", node_id)
        elif new.info is not None:
            rec.info = self._info_to_json(new.info)
        elif old is not None:  # restarted: forget what may have changed
            self.registers.forget(node_id)
            self.scope.forget(node_id)
        self.events.emit("node", node=rec.to_json())

    @staticmethod
    def _info_to_json(info: uavcan.node.GetInfo_1.Response) -> dict[str, Any]:
        return {
            "name": info.name.tobytes().decode("utf8", errors="replace"),
            "software_version": f"{info.software_version.major}.{info.software_version.minor}",
            "hardware_version": f"{info.hardware_version.major}.{info.hardware_version.minor}",
            "software_vcs_revision_id": f"{int(info.software_vcs_revision_id):016x}",
            "unique_id": info.unique_id.tobytes().hex(),
            "certificate_of_authenticity": info.certificate_of_authenticity.tobytes().hex(),
        }

    async def _on_port_list(self, msg: Any, transfer: pycyphal.transport.TransferFrom) -> None:
        if transfer.source_node_id is None:
            return
        rec = self._record(transfer.source_node_id)
        ports = {
            "publishers": _ids_from_list(msg.publishers, 8191),
            "subscribers": _ids_from_list(msg.subscribers, 8191),
            "clients": _ids_from_list(msg.clients.mask, 511),
            "servers": _ids_from_list(msg.servers.mask, 511),
        }
        if ports != rec.ports:
            rec.ports = ports
            self.events.emit("node", node=rec.to_json())

    async def _on_diagnostic(self, msg: uavcan.diagnostic.Record_1, transfer: pycyphal.transport.TransferFrom) -> None:
        entry = {
            "t": float(transfer.timestamp.system),
            "node_id": transfer.source_node_id,
            "severity": SEVERITY.get(msg.severity.value, str(msg.severity.value)),
            "text": msg.text.tobytes().decode("utf8", errors="replace"),
        }
        self.diagnostics.append(entry)
        self.events.emit("diagnostic", **entry)

    # --- Commands -------------------------------------------------------------------------------------------------

    async def execute_command(
        self, node_id: int, command: int | str, parameter: str = "", timeout: float = 2.0
    ) -> dict[str, Any]:
        code = COMMANDS[command] if isinstance(command, str) else int(command)
        if node_id not in self._command_clients:
            self._command_clients[node_id] = self.node.make_client(uavcan.node.ExecuteCommand_1, node_id)
        client = self._command_clients[node_id]
        client.response_timeout = timeout
        request = uavcan.node.ExecuteCommand_1.Request(command=code, parameter=parameter)
        result = await client.call(request)
        if result is None:
            raise TimeoutError(f"Node {node_id} did not answer ExecuteCommand {code}")
        status = int(result[0].status)
        self.events.emit("command", node_id=node_id, command=code, status=COMMAND_STATUS.get(status, str(status)))
        return {"command": code, "status": COMMAND_STATUS.get(status, str(status)), "ok": status == 0}

    async def emergency_stop_all(self) -> dict[int, str]:
        """Stops every publication of the tool, then sends EMERGENCY_STOP to every online node."""
        self.publishers.stop_all(reason="emergency stop")
        targets = [n for n, r in self.nodes.items() if r.online]

        async def stop(node_id: int) -> str:
            try:
                return (await self.execute_command(node_id, "emergency_stop", timeout=0.5))["status"]
            except Exception as ex:
                return f"error: {ex}"

        results = await asyncio.gather(*(stop(n) for n in targets))
        return dict(zip(targets, results, strict=False))

    def status(self) -> dict[str, Any]:
        return {
            "node_id": self.node_id,
            "name": self.node.info.name.tobytes().decode(),
            "version": __version__,
            "pnp": self.pnp_enabled,
            "transport": str(self.node.presentation.transport),
        }
