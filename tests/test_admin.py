# SPDX-License-Identifier: AGPL-3.0-or-later
"""Admin node against the simulated device: node table, registers, ports, commands, diagnostics."""

from __future__ import annotations

import asyncio

import pytest

from cymon_app.admin import Admin
from cymon_app.demo import DemoDevice
from cymon_app.registers import RegisterError

from .conftest import DEVICE_ID, wait_for


async def test_node_table(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    rec = admin.nodes[DEVICE_ID]
    assert rec.online and rec.health == "nominal" and rec.mode == "operational"
    assert rec.info is not None and rec.info["name"] == "org.tecnologic.cymon.demo"
    await wait_for(lambda: rec.ports is not None)
    assert rec.ports is not None
    assert 1000 in rec.ports["publishers"]
    assert {120, 121, 122, 123, 124, 384, 385}.issubset(rec.ports["servers"])


async def test_registers(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    names = await admin.registers.list_names(DEVICE_ID)
    assert "demo.amplitude" in names and "uavcan.srv.cymon_read.type" in names
    assert (await admin.registers.read(DEVICE_ID, "demo.frequency"))["value"] == [50.0]

    written = await admin.registers.write(DEVICE_ID, "demo.amplitude", 2.5)
    assert written == {"name": "demo.amplitude", "kind": "real32", "value": [2.5], "mutable": True, "persistent": False}
    assert device.amplitude == 2.5

    with pytest.raises(RegisterError, match="read-only"):
        await admin.registers.write(DEVICE_ID, "demo.store_count", 1)
    with pytest.raises(RegisterError, match="no register"):
        await admin.registers.write(DEVICE_ID, "nope", 1, kind="real32")
    assert len(await admin.registers.read_all(DEVICE_ID)) == len(names)


async def test_port_map(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    ports = {(p["direction"], p["name"]): p for p in await admin.registers.port_map(DEVICE_ID)}
    assert ports[("pub", "telemetry")]["id"] == 1000
    assert ports[("pub", "telemetry")]["type"] == "uavcan.primitive.array.Real32.1.0"
    assert ports[("sub", "readiness")]["type"] == "reg.udral.service.common.Readiness.0.1"
    assert ports[("srv", "cymon_read")] == {
        "direction": "srv",
        "name": "cymon_read",
        "id": 124,
        "type": "cymon.ReadSamples.0.1",
    }


async def test_commands_and_diagnostics(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    assert await admin.execute_command(DEVICE_ID, "store_persistent_states") == {
        "command": 65530,
        "status": "success",
        "ok": True,
    }
    assert device.store_count == 1
    assert (await admin.execute_command(DEVICE_ID, 1234))["status"] == "bad command"

    device.readiness_deadline = asyncio.get_running_loop().time() + 10
    await device._set_engaged(True, "test")
    assert (await admin.execute_command(DEVICE_ID, "store_persistent_states"))["status"] == "bad state"

    result = await admin.emergency_stop_all()
    assert result == {DEVICE_ID: "success"}
    assert device.estopped and not device.engaged
    await wait_for(lambda: any("emergency stop" in d["text"] for d in admin.diagnostics))
    entry = next(d for d in admin.diagnostics if "emergency stop" in d["text"])
    assert entry["node_id"] == DEVICE_ID and entry["severity"] == "warning"


async def test_command_timeout(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    with pytest.raises(TimeoutError):
        await admin.execute_command(99, "restart", timeout=0.2)


async def test_offline_detection(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    events: list[dict] = []
    async with admin.events.listen() as queue:
        device.close()
        await wait_for(lambda: not admin.nodes[DEVICE_ID].online, timeout=8.0)
        while not queue.empty():
            events.append(queue.get_nowait())
    assert any(e["kind"] == "node" and not e["node"]["online"] for e in events)
