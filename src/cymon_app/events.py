# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fan-out of events (node changes, diagnostics, subject data, capture progress) to the web clients."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator
from typing import Any

Event = dict[str, Any]


class EventHub:
    """Every listener gets every event; a slow listener loses its oldest events instead of blocking the bus."""

    def __init__(self, queue_size: int = 1000) -> None:
        self._queues: set[asyncio.Queue[Event]] = set()
        self._queue_size = queue_size

    def emit(self, kind: str, **data: Any) -> None:
        event = {"kind": kind, **data}
        for queue in self._queues:
            if queue.full():
                with contextlib.suppress(asyncio.QueueEmpty):
                    queue.get_nowait()
            queue.put_nowait(event)

    @contextlib.asynccontextmanager
    async def listen(self) -> AsyncIterator[asyncio.Queue[Event]]:
        queue: asyncio.Queue[Event] = asyncio.Queue(self._queue_size)
        self._queues.add(queue)
        try:
            yield queue
        finally:
            self._queues.discard(queue)

    @property
    def listener_count(self) -> int:
        return len(self._queues)
