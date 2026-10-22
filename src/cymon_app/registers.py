# SPDX-License-Identifier: AGPL-3.0-or-later
"""uavcan.register.List/Access client and JSON conversion of register values."""

from __future__ import annotations

import asyncio
import re
from typing import Any

import pycyphal.application
import uavcan.primitive
import uavcan.register

#: Kinds of uavcan.register.Value.1.0 in union order.
KINDS = (
    "empty",
    "string",
    "unstructured",
    "bit",
    "integer64",
    "integer32",
    "integer16",
    "integer8",
    "natural64",
    "natural32",
    "natural16",
    "natural8",
    "real64",
    "real32",
    "real16",
)

_PORT_REGISTER = re.compile(r"^uavcan\.(pub|sub|srv|cln)\.(.+)\.(id|type)$")


class RegisterError(Exception):
    pass


def value_kind(value: uavcan.register.Value_1_0) -> str:
    for kind in KINDS:
        if getattr(value, kind) is not None:
            return kind
    return "empty"


def value_to_json(value: uavcan.register.Value_1_0) -> Any:
    """Plain JSON value: str for string, hex str for unstructured, list for numbers and bits, None if empty."""
    kind = value_kind(value)
    if kind == "empty":
        return None
    item = getattr(value, kind).value
    if kind == "string":
        return item.tobytes().decode("utf8", errors="replace")
    if kind == "unstructured":
        return item.tobytes().hex()
    if kind == "bit":
        return [bool(b) for b in item]
    if kind.startswith("real"):
        return [float(x) for x in item]
    return [int(x) for x in item]


def json_to_value(kind: str, data: Any) -> uavcan.register.Value_1_0:
    """Builds a value of ``kind`` from a JSON value (scalar or list; str for string, hex str for unstructured)."""
    if kind not in KINDS:
        raise RegisterError(f"Unknown register kind {kind!r}")
    if kind == "empty":
        return uavcan.register.Value_1_0()
    if kind == "string":
        return uavcan.register.Value_1_0(string=uavcan.primitive.String_1_0(str(data)))
    if kind == "unstructured":
        raw = bytes.fromhex(data) if isinstance(data, str) else bytes(data)
        return uavcan.register.Value_1_0(unstructured=uavcan.primitive.Unstructured_1_0(raw))
    items = data if isinstance(data, list) else [data]
    if kind == "bit":
        items = [bool(x) for x in items]
    elif kind.startswith("real"):
        items = [float(x) for x in items]
    else:
        items = [int(x) for x in items]
    return uavcan.register.Value_1_0(**{kind: _array_type(kind)(items)})


def _array_type(kind: str) -> type[Any]:
    name = {"bit": "Bit"}.get(kind, kind.capitalize())
    return getattr(uavcan.primitive.array, f"{name}_1_0")


_INT_RANGES = {
    **{f"integer{b}": (-(2 ** (b - 1)), 2 ** (b - 1) - 1) for b in (8, 16, 32, 64)},
    **{f"natural{b}": (0, 2**b - 1) for b in (8, 16, 32, 64)},
}


def coerce(current: uavcan.register.Value_1_0, data: Any) -> uavcan.register.Value_1_0:
    """Converts a JSON value to the kind and length of the register's current value.

    Numbers are range-checked; an integer register takes integral values only.
    """
    kind = value_kind(current)
    if kind == "empty":
        raise RegisterError("Register is empty; its type is unknown")
    if kind in ("string", "unstructured"):
        return json_to_value(kind, data)
    items = data if isinstance(data, list) else [data]
    length = len(getattr(current, kind).value)
    if len(items) != length:
        raise RegisterError(f"{kind}[{length}] register, got {len(items)} value(s)")
    for item in items:
        if not isinstance(item, (int, float)):
            raise RegisterError(f"{item!r} is not a number")
        if kind in _INT_RANGES:
            low, high = _INT_RANGES[kind]
            if float(item) != int(item) or not low <= int(item) <= high:
                raise RegisterError(f"{item!r} is out of range for {kind} ({low}..{high})")
    return json_to_value(kind, items)


class RegisterClient:
    """Reads and writes the registers of remote nodes."""

    def __init__(self, node: pycyphal.application.Node, timeout: float = 0.5, attempts: int = 3) -> None:
        self._node = node
        self._timeout = timeout
        self._attempts = attempts
        self._clients: dict[tuple[type[Any], int], Any] = {}
        self._locks: dict[int, asyncio.Lock] = {}

    def _client(self, dtype: type[Any], node_id: int) -> Any:
        key = (dtype, node_id)
        if key not in self._clients:
            client = self._node.make_client(dtype, node_id)
            client.response_timeout = self._timeout
            self._clients[key] = client
        return self._clients[key]

    async def _call(self, dtype: type[Any], node_id: int, request: Any) -> Any:
        client = self._client(dtype, node_id)
        for _ in range(self._attempts):
            result = await client.call(request)
            if result is not None:
                return result[0]
        raise RegisterError(f"Node {node_id} did not answer {dtype.__name__}")

    async def list_names(self, node_id: int) -> list[str]:
        names: list[str] = []
        async with self._lock(node_id):
            for index in range(65536):
                response = await self._call(uavcan.register.List_1_0, node_id, uavcan.register.List_1_0.Request(index))
                name = response.name.name.tobytes().decode("utf8", errors="replace")
                if not name:
                    break
                names.append(name)
        return names

    async def read(self, node_id: int, name: str) -> dict[str, Any]:
        response = await self._access(node_id, name, uavcan.register.Value_1_0())
        return self._entry(name, response)

    async def write(self, node_id: int, name: str, data: Any, kind: str | None = None) -> dict[str, Any]:
        """Writes ``data`` converted to the current kind (or to ``kind``); returns the value after the write."""
        if kind is None:
            current = await self._access(node_id, name, uavcan.register.Value_1_0())
            if not current.mutable:
                raise RegisterError(f"{name} is read-only")
            value = coerce(current.value, data)
        else:
            value = json_to_value(kind, data)
        response = await self._access(node_id, name, value)
        return self._entry(name, response)

    async def read_all(self, node_id: int) -> list[dict[str, Any]]:
        return [await self.read(node_id, name) for name in await self.list_names(node_id)]

    async def port_map(self, node_id: int, names: list[str] | None = None) -> list[dict[str, Any]]:
        """Ports configured through the standard registers ``uavcan.{pub,sub,srv,cln}.<name>.{id,type}``."""
        names = names if names is not None else await self.list_names(node_id)
        ports: dict[tuple[str, str], dict[str, Any]] = {}
        for name in names:
            match = _PORT_REGISTER.match(name)
            if not match:
                continue
            direction, port_name, field = match.groups()
            entry = ports.setdefault(
                (direction, port_name), {"direction": direction, "name": port_name, "id": None, "type": None}
            )
            value = value_to_json((await self._access(node_id, name, uavcan.register.Value_1_0())).value)
            if field == "id":
                port_id = value[0] if isinstance(value, list) and value else None
                limit = 8191 if direction in ("pub", "sub") else 511
                entry["id"] = port_id if port_id is not None and 0 <= port_id <= limit else None
            else:
                entry["type"] = value if isinstance(value, str) and value else None
        return sorted(ports.values(), key=lambda p: (p["direction"], p["name"]))

    async def _access(self, node_id: int, name: str, value: uavcan.register.Value_1_0) -> Any:
        request = uavcan.register.Access_1_0.Request(name=uavcan.register.Name_1_0(name), value=value)
        async with self._lock(node_id):
            response = await self._call(uavcan.register.Access_1_0, node_id, request)
        if value_kind(response.value) == "empty" and value_kind(value) != "empty":
            raise RegisterError(f"Node {node_id} has no register {name}")
        return response

    def _lock(self, node_id: int) -> asyncio.Lock:
        # One request at a time per node: small embedded nodes answer them in order.
        return self._locks.setdefault(node_id, asyncio.Lock())

    @staticmethod
    def _entry(name: str, response: Any) -> dict[str, Any]:
        return {
            "name": name,
            "kind": value_kind(response.value),
            "value": value_to_json(response.value),
            "mutable": bool(response.mutable),
            "persistent": bool(response.persistent),
        }

    def forget(self, node_id: int) -> None:
        for key in [k for k in self._clients if k[1] == node_id]:
            self._clients.pop(key).close()
