# SPDX-License-Identifier: AGPL-3.0-or-later
"""REST API in-process and the WebSocket dead-man over a real server."""

from __future__ import annotations

import asyncio
import json
import socket
from pathlib import Path

import httpx
import uvicorn
import websockets

from cymon_app.admin import Admin
from cymon_app.demo import READINESS_SUBJECT, DemoDevice
from cymon_app.session import Session
from cymon_app.web.server import CLIENT_HEADER, create_app

from .conftest import DEVICE_ID, wait_for


def _client(session: Session) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.ASGITransport(app=create_app(session)), base_url="http://test")


async def test_rest(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    async with _client(Session(admin)) as http:
        status = (await http.get("/api/status")).json()
        assert status["connected"] and status["node_id"] == 127
        assert [n["node_id"] for n in (await http.get("/api/nodes")).json()] == [DEVICE_ID]
        assert (await http.get("/api/nodes/99/registers")).status_code == 404

        r = await http.put(f"/api/nodes/{DEVICE_ID}/registers/demo.frequency", json={"value": [10]})
        assert r.status_code == 200 and r.json()["value"] == [10.0] and device.frequency == 10.0
        r = await http.put(f"/api/nodes/{DEVICE_ID}/registers/demo.frequency", json={"value": "x"})
        assert r.status_code == 400 and "not a number" in r.json()["detail"]

        r = await http.post(f"/api/nodes/{DEVICE_ID}/command", json={"command": "store_persistent_states"})
        assert r.json()["ok"]
        assert (await http.post(f"/api/nodes/{DEVICE_ID}/command", json={"command": "nope"})).status_code == 400

        types = (await http.get("/api/types")).json()
        assert "cymon.TriggerEvent.0.1" in types and "uavcan.node.Heartbeat.1.0" in types
        assert (await http.get("/api/types/reg.udral.service.common.Readiness.0.1")).json()["default"] == {"value": 0}
        assert (await http.get("/api/types/no.such.Type.1.0")).status_code == 400

        r = await http.post(
            f"/api/nodes/{DEVICE_ID}/scope/capture", json={"channels": [0], "sample_period_us": 100, "num_samples": 20}
        )
        assert r.status_code == 200 and len(r.json()["t"]) == 20
        assert (await http.post(f"/api/nodes/{DEVICE_ID}/scope/capture", json={"channels": []})).status_code == 422

        r = await http.post("/api/subjects", json={"subject_id": 1000, "type": "uavcan.primitive.array.Real32.1.0"})
        assert (await http.delete(f"/api/subjects/{r.json()['id']}")).json() == {"ok": True}

        assert (await http.get("/index.html")).status_code == 200
        assert (await http.get("/vendor/uPlot.iife.min.js")).status_code == 200


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


async def test_websocket_disconnect_stops_publications(bus: tuple[Admin, DemoDevice]) -> None:
    admin, device = bus
    port = _free_port()
    config = uvicorn.Config(create_app(Session(admin)), host="127.0.0.1", port=port, log_level="warning")
    server = uvicorn.Server(config)
    task = asyncio.create_task(server.serve())
    await wait_for(lambda: server.started)
    try:
        async with websockets.connect(f"ws://127.0.0.1:{port}/ws?client=tab1") as ws:
            hello = json.loads(await ws.recv())
            assert hello["kind"] == "hello" and hello["client"] == "tab1"
            async with httpx.AsyncClient(base_url=f"http://127.0.0.1:{port}") as http:
                r = await http.post(
                    "/api/publishers",
                    headers={CLIENT_HEADER: "tab1"},
                    json={
                        "subject_id": READINESS_SUBJECT,
                        "type": "reg.udral.service.common.Readiness.0.1",
                        "value": {"value": 3},
                        "rate_hz": 20,
                        "lease_s": 0.5,
                    },
                )
                assert r.status_code == 200
            for _ in range(10):  # renewed by the page: survives several leases
                await ws.send(json.dumps({"type": "renew"}))
                await asyncio.sleep(0.15)
            assert device.engaged and admin.publishers.list()
        await wait_for(lambda: not admin.publishers.list(), timeout=2.0)
        await wait_for(lambda: not device.engaged, timeout=2.0)
    finally:
        server.should_exit = True
        await task


async def test_connect_from_the_ui(tmp_path: Path) -> None:
    """Started without a bus: load DSDL, connect to simulated devices, disconnect."""
    session = Session()
    try:
        async with _client(session) as http:
            status = (await http.get("/api/status")).json()
            assert not status["connected"] and "cymon" in status["dsdl"]["namespaces"]
            assert (await http.get("/api/nodes")).status_code == 503
            assert (await http.get("/api/adapters")).status_code == 200
            assert (await http.post("/api/connect", json={"interfaces": []})).status_code == 400

            namespace = tmp_path / "testns"
            (namespace / "sub").mkdir(parents=True)
            (namespace / "sub" / "Thing.1.0.dsdl").write_text("float32 value\n@sealed\n")
            r = await http.put("/api/dsdl", json={"roots": [str(namespace)]})
            assert r.status_code == 200 and "testns" in r.json()["namespaces"]
            assert "testns.sub.Thing.1.0" in (await http.get("/api/types")).json()
            assert (await http.put("/api/dsdl", json={"roots": [str(tmp_path / "nope")]})).status_code == 400
            assert (await http.put("/api/dsdl", json={"roots": []})).json()["roots"] == []
            assert "testns.sub.Thing.1.0" not in (await http.get("/api/types")).json()

            r = await http.post("/api/connect", json={"demo_devices": 1})
            assert r.status_code == 200 and r.json()["connected"] and r.json()["config"]["demo_devices"] == 1
            await wait_for(lambda: DEVICE_ID in session.admin.nodes)  # type: ignore[union-attr]
            assert [n["node_id"] for n in (await http.get("/api/nodes")).json()] == [DEVICE_ID]

            assert not (await http.post("/api/disconnect")).json()["connected"]
            assert (await http.get("/api/nodes")).status_code == 503
    finally:
        session.close()
