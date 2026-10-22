# SPDX-License-Identifier: AGPL-3.0-or-later
"""The bus connection of the running tool, which the web UI can change.

``cymon`` without an interface starts with no bus: the web UI loads DSDL,
picks an interface and connects. :class:`Session` owns the admin node of the
current connection and the event hub, which outlives connections so that open
WebSockets keep working.

Import only after :func:`cymon_app.dsdl.setup`.
"""

from __future__ import annotations

import asyncio
import dataclasses
import logging
from collections.abc import Iterable
from typing import Any

from . import __version__, dsdl
from .admin import Admin
from .events import EventHub
from .transport import CanSettings, describe, make_transport

_logger = logging.getLogger(__name__)

#: Virtual bus of the simulated devices (``cymon demo``).
DEMO_CHANNEL = "virtual:cymon-demo"


@dataclasses.dataclass
class BusConfig:
    #: Interfaces as ``kind:channel``; several form a redundant bus. Ignored with ``demo_devices``.
    interfaces: list[str] = dataclasses.field(default_factory=list)
    node_id: int = 127
    bitrate: int = 1_000_000
    data_bitrate: int | None = None
    classic: bool = False
    #: Plug-and-play allocator database, None: no allocator.
    pnp: str | None = None
    #: Simulated devices on a virtual bus instead of the interfaces (node-IDs 10, 11, ...).
    demo_devices: int = 0

    @property
    def settings(self) -> CanSettings:
        return CanSettings(self.bitrate, self.data_bitrate, 8 if self.classic else 64)


class Session:
    """Connects to and disconnects from the bus; loads DSDL."""

    def __init__(self, admin: Admin | None = None, dsdl_roots: Iterable[str] = ()) -> None:
        self.admin = admin
        self.events = admin.events if admin else EventHub()
        self.config: BusConfig | None = None
        self.dsdl_roots = list(dsdl_roots)
        self._devices: list[Any] = []
        #: Counts connections, so that clients notice a reconnect with the same settings.
        self._connection = 1 if admin else 0
        self._lock = asyncio.Lock()

    @property
    def connected(self) -> bool:
        return self.admin is not None

    async def connect(self, config: BusConfig) -> dict[str, Any]:
        """Closes the current connection, if any, and opens one with ``config``."""
        async with self._lock:
            self._close()
            if config.demo_devices:
                self._connect_demo(config)
            else:
                transport = make_transport(config.interfaces, config.node_id, config.settings)
                _logger.info("Bus: %s", describe(config.interfaces, config.settings))
                try:
                    self.admin = Admin(transport, pnp_database=config.pnp, events=self.events)
                except Exception:
                    transport.close()
                    raise
                self.admin.start()
            self.config = config
            self._connection += 1
            _logger.info("Node-ID %s on %s", self.admin.node_id, self.admin.status()["transport"])
            self._changed()
        return self.status()

    def _connect_demo(self, config: BusConfig) -> None:
        from .demo import DemoDevice

        self._devices = [
            DemoDevice(make_transport([DEMO_CHANNEL], 10 + i), name=f"org.tecnologic.cymon.demo{i}")
            for i in range(config.demo_devices)
        ]
        self.admin = Admin(make_transport([DEMO_CHANNEL], config.node_id), pnp_database=config.pnp, events=self.events)
        self.admin.start()
        for device in self._devices:
            device.start()

    async def disconnect(self) -> dict[str, Any]:
        async with self._lock:
            if self.connected:
                self._close()
                self._changed()
        return self.status()

    def _close(self) -> None:
        for device in self._devices:
            device.close()
        self._devices = []
        if self.admin is not None:
            self.admin.close()
            self.admin = None
            self.config = None

    async def load_dsdl(self, roots: list[str]) -> dict[str, Any]:
        """Compiles and loads these extra DSDL root namespaces (replacing the previous ones)."""
        async with self._lock:
            await asyncio.to_thread(dsdl.compile_roots, roots)
            dsdl.setup(roots)
            self.dsdl_roots = list(roots)
            self._changed()
        return self.status()

    def close(self) -> None:
        self._close()

    def _changed(self) -> None:
        self.events.emit("session", status=self.status())

    def status(self) -> dict[str, Any]:
        status: dict[str, Any] = {
            "version": __version__,
            "connected": self.connected,
            "connection": self._connection,
            "config": dataclasses.asdict(self.config) if self.config else None,
            "dsdl": {"roots": self.dsdl_roots, "namespaces": dsdl.namespaces()},
        }
        if self.admin is not None:
            status.update(self.admin.status())
        return status
