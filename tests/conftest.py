# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fixtures: compiled DSDL and an admin node plus simulated devices on a virtual CAN bus."""

from __future__ import annotations

import asyncio
import itertools
from collections.abc import AsyncIterator
from typing import Any

import pytest

from cymon_app import dsdl

dsdl.setup()  # before any module that imports pycyphal.application

from cymon_app.admin import Admin  # noqa: E402
from cymon_app.demo import DemoDevice  # noqa: E402
from cymon_app.transport import make_transport  # noqa: E402

_bus_ids = itertools.count()
DEVICE_ID = 10


async def wait_for(predicate: Any, timeout: float = 5.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() > deadline:
            raise TimeoutError("condition not met")
        await asyncio.sleep(0.02)


@pytest.fixture
def channel() -> str:
    """A virtual CAN bus of its own for each test."""
    return f"virtual:test{next(_bus_ids)}"


@pytest.fixture
async def bus(channel: str) -> AsyncIterator[tuple[Admin, DemoDevice]]:
    """A started admin node (127) and a demo device (10) that has answered GetInfo."""
    device = DemoDevice(make_transport([channel], DEVICE_ID))
    admin = Admin(make_transport([channel], 127), unique_id=bytes(16))
    device.start()
    admin.start()
    await wait_for(lambda: DEVICE_ID in admin.nodes and admin.nodes[DEVICE_ID].info is not None)
    yield admin, device
    admin.close()
    device.close()
