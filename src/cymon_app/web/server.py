# SPDX-License-Identifier: AGPL-3.0-or-later
"""REST and WebSocket API plus the static web UI.

Import only after :func:`cymon_app.dsdl.setup`.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import uuid
from pathlib import Path
from typing import Any

import pycyphal.dsdl
import pydsdl
from fastapi import Body, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .. import dsdl
from ..admin import COMMANDS, Admin
from ..registers import RegisterError
from ..scope import CaptureRequest, ScopeError

_logger = logging.getLogger(__name__)
STATIC_DIR = Path(__file__).parent / "static"

#: Header that ties publications to the WebSocket of the browser tab that started them.
CLIENT_HEADER = "X-Cymon-Client"


class RegisterWrite(BaseModel):
    value: Any
    kind: str | None = None


class Command(BaseModel):
    command: int | str
    parameter: str = ""


class SubjectRequest(BaseModel):
    subject_id: int = Field(ge=0, le=8191)
    type: str


class PublishRequest(BaseModel):
    subject_id: int = Field(ge=0, le=8191)
    type: str
    value: dict[str, Any] = {}
    rate_hz: float = Field(10.0, ge=0.1, le=1000.0)
    lease_s: float = Field(1.0, ge=0.1, le=60.0)
    priority: str = "nominal"


class CaptureBody(BaseModel):
    channels: list[int] = Field(min_length=1, max_length=16)
    sample_period_us: float = Field(gt=0)
    num_samples: int = Field(ge=1, le=65535)
    pretrigger_samples: int = Field(0, ge=0, le=65534)
    trigger_mode: str = "immediate"
    trigger_source: int = Field(0, ge=0, le=255)
    trigger_level: float = 0.0
    hysteresis_band: float = 0.0
    timeout_s: float = Field(30.0, gt=0, le=3600)


def _message_types() -> list[str]:
    names: list[str] = []
    roots = dsdl._compiled_roots
    for root in roots:
        for t in pydsdl.read_namespace(root, [r for r in roots if r != root]):
            if not isinstance(t, pydsdl.ServiceType):
                names.append(f"{t.full_name}.{t.version.major}.{t.version.minor}")
    return sorted(names)


def create_app(admin: Admin) -> FastAPI:
    app = FastAPI(title="cymon", version=admin.status()["version"])
    app.state.admin = admin
    type_cache: list[str] = []

    @app.exception_handler(RegisterError)
    @app.exception_handler(ScopeError)
    @app.exception_handler(LookupError)
    @app.exception_handler(ValueError)
    async def bad_request(_: Request, exc: Exception) -> JSONResponse:
        return JSONResponse({"detail": str(exc)}, status_code=400)

    @app.exception_handler(TimeoutError)
    async def timeout(_: Request, exc: Exception) -> JSONResponse:
        return JSONResponse({"detail": str(exc)}, status_code=504)

    # --- Network -----------------------------------------------------------------------------------------------

    @app.get("/api/status")
    async def status() -> dict[str, Any]:
        return admin.status()

    @app.get("/api/nodes")
    async def nodes() -> list[dict[str, Any]]:
        return [r.to_json() for _, r in sorted(admin.nodes.items())]

    def _known(node_id: int) -> None:
        if node_id not in admin.nodes:
            raise HTTPException(404, f"Node {node_id} not seen on the bus")

    @app.get("/api/nodes/{node_id}/registers")
    async def registers(node_id: int) -> list[dict[str, Any]]:
        _known(node_id)
        return await admin.registers.read_all(node_id)

    @app.get("/api/nodes/{node_id}/registers/{name}")
    async def read_register(node_id: int, name: str) -> dict[str, Any]:
        _known(node_id)
        return await admin.registers.read(node_id, name)

    @app.put("/api/nodes/{node_id}/registers/{name}")
    async def write_register(node_id: int, name: str, body: RegisterWrite) -> dict[str, Any]:
        _known(node_id)
        return await admin.registers.write(node_id, name, body.value, body.kind)

    @app.get("/api/nodes/{node_id}/ports")
    async def ports(node_id: int) -> list[dict[str, Any]]:
        _known(node_id)
        return await admin.registers.port_map(node_id)

    @app.get("/api/commands")
    async def commands() -> dict[str, int]:
        return COMMANDS

    @app.post("/api/nodes/{node_id}/command")
    async def command(node_id: int, body: Command) -> dict[str, Any]:
        _known(node_id)
        if isinstance(body.command, str) and body.command not in COMMANDS:
            raise HTTPException(400, f"Unknown command {body.command!r}")
        return await admin.execute_command(node_id, body.command, body.parameter)

    @app.post("/api/estop")
    async def estop() -> dict[str, Any]:
        return {"nodes": await admin.emergency_stop_all()}

    @app.get("/api/diagnostics")
    async def diagnostics() -> list[dict[str, Any]]:
        return list(admin.diagnostics)

    # --- Types, subjects, publications -------------------------------------------------------------------------

    @app.get("/api/types")
    async def types() -> list[str]:
        if not type_cache:
            type_cache.extend(await asyncio.to_thread(_message_types))
        return type_cache

    @app.get("/api/types/{name}")
    async def type_template(name: str) -> dict[str, Any]:
        dtype = dsdl.find_type(name)
        return {"type": dsdl.type_name(dtype), "default": pycyphal.dsdl.to_builtin(dtype())}

    @app.get("/api/subjects")
    async def subjects() -> list[dict[str, Any]]:
        return admin.subjects.list()

    @app.post("/api/subjects")
    async def subscribe(body: SubjectRequest) -> dict[str, Any]:
        return admin.subjects.subscribe(body.subject_id, body.type)

    @app.delete("/api/subjects/{sub_id}")
    async def unsubscribe(sub_id: int) -> dict[str, Any]:
        admin.subjects.unsubscribe(sub_id)
        return {"ok": True}

    @app.get("/api/publishers")
    async def publishers() -> list[dict[str, Any]]:
        return admin.publishers.list()

    @app.post("/api/publishers")
    async def publish(body: PublishRequest, request: Request) -> dict[str, Any]:
        owner = request.headers.get(CLIENT_HEADER, "")
        return admin.publishers.start(
            body.subject_id, body.type, body.value, body.rate_hz, body.lease_s, body.priority, owner=owner
        )

    @app.put("/api/publishers/{pub_id}")
    async def update_publication(pub_id: int, value: dict[str, Any] = Body(...)) -> dict[str, Any]:
        try:
            return admin.publishers.update(pub_id, value)
        except KeyError:
            raise HTTPException(404, f"No publication {pub_id}") from None

    @app.delete("/api/publishers/{pub_id}")
    async def stop_publication(pub_id: int) -> dict[str, Any]:
        admin.publishers.stop(pub_id)
        return {"ok": True}

    @app.post("/api/publishers/stop")
    async def stop_publications() -> dict[str, Any]:
        return {"stopped": admin.publishers.stop_all()}

    # --- Scope -------------------------------------------------------------------------------------------------

    @app.get("/api/nodes/{node_id}/scope")
    async def scope_info(node_id: int, refresh: bool = False) -> dict[str, Any]:
        _known(node_id)
        await admin.scope.discover(node_id, refresh=refresh)
        return await admin.scope.info(node_id)

    @app.get("/api/nodes/{node_id}/scope/status")
    async def scope_status(node_id: int) -> dict[str, Any]:
        _known(node_id)
        return await admin.scope.status(node_id)

    @app.post("/api/nodes/{node_id}/scope/capture")
    async def scope_capture(node_id: int, body: CaptureBody) -> dict[str, Any]:
        _known(node_id)
        return await admin.scope.capture(node_id, CaptureRequest(**body.model_dump()))

    @app.post("/api/nodes/{node_id}/scope/read")
    async def scope_read(node_id: int) -> dict[str, Any]:
        _known(node_id)
        return await admin.scope.read(node_id)

    @app.post("/api/nodes/{node_id}/scope/disarm")
    async def scope_disarm(node_id: int) -> dict[str, Any]:
        _known(node_id)
        await admin.scope.disarm(node_id)
        return {"ok": True}

    # --- Live events -------------------------------------------------------------------------------------------

    @app.websocket("/ws")
    async def events(ws: WebSocket) -> None:
        """Pushes every event as JSON. The client sends ``{"type": "renew"}`` to keep its publications alive."""
        await ws.accept()
        client_id = ws.query_params.get("client") or uuid.uuid4().hex
        await ws.send_json({"kind": "hello", "client": client_id, "status": admin.status()})

        async def receive() -> None:
            while True:
                message = json.loads(await ws.receive_text())
                if message.get("type") == "renew":
                    admin.publishers.renew(owner=client_id)

        async with admin.events.listen() as queue:
            receiver = asyncio.create_task(receive())
            try:
                while not receiver.done():
                    getter = asyncio.create_task(queue.get())
                    done, _ = await asyncio.wait({getter, receiver}, return_when=asyncio.FIRST_COMPLETED)
                    if getter in done:
                        await ws.send_text(json.dumps(getter.result(), default=_json_default))
                    else:
                        getter.cancel()
            except (WebSocketDisconnect, RuntimeError):
                pass
            finally:
                receiver.cancel()
                with contextlib.suppress(asyncio.CancelledError, WebSocketDisconnect, RuntimeError):
                    await receiver
                stopped = admin.publishers.stop_all(owner=client_id, reason="client disconnected")
                if stopped:
                    _logger.warning("Client %s disconnected: stopped %d publication(s)", client_id, stopped)

    app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")
    return app


def _json_default(value: Any) -> Any:
    try:
        return value.item()  # numpy scalars
    except AttributeError:
        return str(value)
