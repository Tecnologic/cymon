# SPDX-License-Identifier: AGPL-3.0-or-later
"""Live subject monitoring and periodic publishing with a dead-man lease."""

from __future__ import annotations

import asyncio
import dataclasses
import itertools
import logging
import time
from typing import Any

import numpy as np
import pycyphal.application
import pycyphal.dsdl
import pycyphal.transport

from . import dsdl
from .events import EventHub

_logger = logging.getLogger(__name__)


def flatten(value: Any, prefix: str = "") -> dict[str, float]:
    """Numeric leaves of a DSDL object as ``{"a.b[2]": 1.0}``; strings and byte arrays are skipped."""
    out: dict[str, float] = {}
    if isinstance(value, dict):
        for key, item in value.items():
            out.update(flatten(item, f"{prefix}.{key}" if prefix else key))
    elif isinstance(value, (list, tuple)):
        if len(value) <= 64:
            for i, item in enumerate(value):
                out.update(flatten(item, f"{prefix}[{i}]"))
    elif isinstance(value, (bool, int, float)):
        out[prefix] = float(value)
    return out


def _builtin(message: Any) -> Any:
    return _jsonable(pycyphal.dsdl.to_builtin(message))


@dataclasses.dataclass
class Subscription:
    sub_id: int
    subject_id: int
    type_name: str
    subscriber: Any
    count: int = 0
    pending: list[dict[str, Any]] = dataclasses.field(default_factory=list)


class SubjectMonitor:
    """Subscribes to subjects by ID and type and streams their numeric fields in batches."""

    def __init__(self, node: pycyphal.application.Node, events: EventHub, batch_period: float = 0.05) -> None:
        self._node = node
        self._events = events
        self._batch_period = batch_period
        self._subs: dict[int, Subscription] = {}
        self._ids = itertools.count(1)
        self._task: asyncio.Task[None] | None = None

    def subscribe(self, subject_id: int, type_name: str) -> dict[str, Any]:
        for sub in self._subs.values():
            if sub.subject_id == subject_id and sub.type_name == type_name:
                return self._describe(sub)
        dtype = dsdl.find_type(type_name)
        full_name = dsdl.type_name(dtype)
        subscriber = self._node.make_subscriber(dtype, subject_id)
        sub = Subscription(next(self._ids), subject_id, full_name, subscriber)
        self._subs[sub.sub_id] = sub

        async def handler(message: Any, transfer: pycyphal.transport.TransferFrom) -> None:
            sub.count += 1
            data = _builtin(message)
            sub.pending.append(
                {
                    "t": float(transfer.timestamp.system),
                    "source": transfer.source_node_id,
                    "fields": flatten(data),
                    "data": data,
                }
            )

        subscriber.receive_in_background(handler)
        if self._task is None:
            self._task = asyncio.get_running_loop().create_task(self._flush_loop())
        return self._describe(sub)

    def unsubscribe(self, sub_id: int) -> None:
        sub = self._subs.pop(sub_id, None)
        if sub:
            sub.subscriber.close()

    def list(self) -> list[dict[str, Any]]:
        return [self._describe(s) for s in self._subs.values()]

    @staticmethod
    def _describe(sub: Subscription) -> dict[str, Any]:
        return {"id": sub.sub_id, "subject_id": sub.subject_id, "type": sub.type_name, "count": sub.count}

    async def _flush_loop(self) -> None:
        while True:
            await asyncio.sleep(self._batch_period)
            for sub in self._subs.values():
                if sub.pending:
                    samples, sub.pending = sub.pending, []
                    # The structure of the last message is enough for a tree view.
                    self._events.emit(
                        "subject",
                        id=sub.sub_id,
                        subject_id=sub.subject_id,
                        type=sub.type_name,
                        t=[s["t"] for s in samples],
                        source=[s["source"] for s in samples],
                        fields={key: [s["fields"].get(key) for s in samples] for key in samples[-1]["fields"]},
                        last=samples[-1]["data"],
                    )

    def close(self) -> None:
        if self._task:
            self._task.cancel()
        for sub_id in list(self._subs):
            self.unsubscribe(sub_id)


@dataclasses.dataclass
class Publication:
    pub_id: int
    subject_id: int
    type_name: str
    message: Any
    rate_hz: float
    lease_s: float
    publisher: Any
    expires: float
    sent: int = 0
    owner: str = ""
    task: asyncio.Task[None] | None = None


class PublisherManager:
    """Periodic publications that stop on their own unless their lease is renewed.

    The web client renews the leases of its publications several times a
    second. When the browser closes, loses the connection or hangs, the
    publications stop within ``lease_s``; a drive that needs a periodic
    readiness or setpoint then falls back to its safe state by itself.
    """

    def __init__(self, node: pycyphal.application.Node, events: EventHub) -> None:
        self._node = node
        self._events = events
        self._pubs: dict[int, Publication] = {}
        self._ids = itertools.count(1)

    def start(
        self,
        subject_id: int,
        type_name: str,
        value: dict[str, Any] | None,
        rate_hz: float,
        lease_s: float = 1.0,
        priority: str = "nominal",
        owner: str = "",
    ) -> dict[str, Any]:
        if not 0.1 <= rate_hz <= 1000.0:
            raise ValueError("rate_hz must be within 0.1..1000")
        if not 0.1 <= lease_s <= 60.0:
            raise ValueError("lease_s must be within 0.1..60")
        dtype = dsdl.find_type(type_name)
        message = pycyphal.dsdl.update_from_builtin(dtype(), value or {})
        publisher = self._node.make_publisher(dtype, subject_id)
        publisher.priority = pycyphal.transport.Priority[priority.upper()]
        publisher.send_timeout = min(1.0, 1.0 / rate_hz)
        pub = Publication(
            next(self._ids),
            subject_id,
            dsdl.type_name(dtype),
            message,
            rate_hz,
            lease_s,
            publisher,
            time.monotonic() + lease_s,
            owner=owner,
        )
        self._pubs[pub.pub_id] = pub
        pub.task = asyncio.get_running_loop().create_task(self._run(pub))
        self._events.emit("publisher", action="started", **self._describe(pub))
        return self._describe(pub)

    def update(self, pub_id: int, value: dict[str, Any]) -> dict[str, Any]:
        pub = self._pubs[pub_id]
        dtype = type(pub.message)
        pub.message = pycyphal.dsdl.update_from_builtin(dtype(), value)
        return self._describe(pub)

    def renew(self, owner: str | None = None, pub_id: int | None = None) -> None:
        now = time.monotonic()
        for pub in self._pubs.values():
            if (pub_id is None or pub.pub_id == pub_id) and (owner is None or pub.owner == owner):
                pub.expires = now + pub.lease_s

    def stop(self, pub_id: int, reason: str = "stopped") -> None:
        pub = self._pubs.pop(pub_id, None)
        if pub is None:
            return
        if pub.task and pub.task is not asyncio.current_task():
            pub.task.cancel()
        pub.publisher.close()
        self._events.emit("publisher", action="stopped", reason=reason, **self._describe(pub))

    def stop_all(self, owner: str | None = None, reason: str = "stopped") -> int:
        ids = [p.pub_id for p in self._pubs.values() if owner is None or p.owner == owner]
        for pub_id in ids:
            self.stop(pub_id, reason)
        return len(ids)

    def list(self) -> list[dict[str, Any]]:
        return [self._describe(p) for p in self._pubs.values()]

    async def _run(self, pub: Publication) -> None:
        period = 1.0 / pub.rate_hz
        next_time = time.monotonic()
        while True:
            now = time.monotonic()
            if now > pub.expires:
                _logger.warning("Publication %d on subject %d: lease expired, stopping", pub.pub_id, pub.subject_id)
                self.stop(pub.pub_id, reason="lease expired")
                return
            try:
                if await pub.publisher.publish(pub.message):
                    pub.sent += 1
            except Exception as ex:  # transport errors must not kill the loop silently
                _logger.error("Publication %d failed: %s", pub.pub_id, ex)
            next_time += period
            await asyncio.sleep(max(0.0, next_time - time.monotonic()))

    @staticmethod
    def _describe(pub: Publication) -> dict[str, Any]:
        return {
            "id": pub.pub_id,
            "subject_id": pub.subject_id,
            "type": pub.type_name,
            "rate_hz": pub.rate_hz,
            "lease_s": pub.lease_s,
            "sent": pub.sent,
            "value": _builtin(pub.message),
        }


def _jsonable(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, np.generic):
        return value.item()
    return value
