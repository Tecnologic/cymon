# SPDX-License-Identifier: AGPL-3.0-or-later
"""Subject monitoring and publications with leases."""

from __future__ import annotations

import asyncio

import pytest

from cymon_app.admin import Admin
from cymon_app.demo import READINESS_SUBJECT, TELEMETRY_SUBJECT, DemoDevice

from .conftest import wait_for

READINESS = "reg.udral.service.common.Readiness.0.1"


async def test_monitor_streams_batches(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    async with admin.events.listen() as queue:
        sub = admin.subjects.subscribe(TELEMETRY_SUBJECT, "uavcan.primitive.array.Real32.1.0")
        assert admin.subjects.subscribe(TELEMETRY_SUBJECT, "uavcan.primitive.array.Real32.1.0")["id"] == sub["id"]
        event = None
        while event is None or event["kind"] != "subject":
            event = await asyncio.wait_for(queue.get(), 3.0)
    assert event["subject_id"] == TELEMETRY_SUBJECT and event["source"][0] == 10
    assert set(event["fields"]) == {"value[0]", "value[1]", "value[2]"}
    assert len(event["t"]) == len(event["fields"]["value[0]"])
    admin.subjects.unsubscribe(sub["id"])
    assert admin.subjects.list() == []


async def test_publication_engages_until_stopped(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    pub = admin.publishers.start(READINESS_SUBJECT, READINESS, {"value": 3}, rate_hz=20, lease_s=5.0, owner="tab")
    await wait_for(lambda: device.engaged)
    admin.publishers.stop_all(owner="other")
    assert admin.publishers.list()[0]["id"] == pub["id"]
    admin.publishers.update(pub["id"], {"value": 2})  # STANDBY
    await wait_for(lambda: not device.engaged)
    admin.publishers.update(pub["id"], {"value": 3})
    await wait_for(lambda: device.engaged)
    assert admin.publishers.stop_all(owner="tab") == 1
    await wait_for(lambda: not device.engaged, timeout=2.0)  # the device's own readiness timeout


async def test_lease_expires_without_renewal(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    async with admin.events.listen() as queue:
        admin.publishers.start(READINESS_SUBJECT, READINESS, {"value": 3}, rate_hz=20, lease_s=0.3)
        await wait_for(lambda: device.engaged)
        for _ in range(5):  # renewed for a while ...
            admin.publishers.renew()
            await asyncio.sleep(0.1)
        assert admin.publishers.list()
        await wait_for(lambda: not admin.publishers.list(), timeout=1.0)  # ... then not
        stops = []
        while not queue.empty():
            event = queue.get_nowait()
            if event["kind"] == "publisher" and event["action"] == "stopped":
                stops.append(event)
    assert stops[0]["reason"] == "lease expired"
    await wait_for(lambda: not device.engaged, timeout=2.0)


async def test_bad_publication_requests(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    with pytest.raises(ValueError):
        admin.publishers.start(READINESS_SUBJECT, READINESS, {}, rate_hz=5000)
    with pytest.raises(LookupError):
        admin.publishers.start(READINESS_SUBJECT, "reg.nope.Type.1.0", {}, rate_hz=1)
    with pytest.raises(Exception):  # noqa: B017 - pycyphal raises its own error for unknown fields
        admin.publishers.start(READINESS_SUBJECT, READINESS, {"nope": 1}, rate_hz=1)
