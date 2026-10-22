# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pure functions: DSDL lookup, register value conversion, flattening, CLI parsing."""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import uavcan.register

from cymon_app import __version__, dsdl
from cymon_app.cli import _parser
from cymon_app.registers import RegisterError, coerce, json_to_value, value_kind, value_to_json
from cymon_app.subjects import flatten


def test_find_type_with_and_without_version() -> None:
    assert dsdl.type_name(dsdl.find_type("cymon.ReadSamples.0.1")) == "cymon.ReadSamples.0.1"
    assert dsdl.type_name(dsdl.find_type("uavcan.node.Heartbeat")) == "uavcan.node.Heartbeat.1.0"
    assert dsdl.type_name(dsdl.find_type("uavcan.diagnostic.Record")) == "uavcan.diagnostic.Record.1.1"
    with pytest.raises(LookupError):
        dsdl.find_type("cymon.Nope.0.1")
    with pytest.raises(LookupError):
        dsdl.find_type("nonexistent.Type")


@pytest.mark.parametrize(
    ("kind", "data", "expected"),
    [
        ("real32", 1.5, [1.5]),
        ("natural16", [1, 2], [1, 2]),
        ("integer8", -3, [-3]),
        ("bit", [True, False], [True, False]),
        ("string", "abc", "abc"),
        ("unstructured", "0aff", "0aff"),
        ("empty", None, None),
    ],
)
def test_json_round_trip(kind: str, data: object, expected: object) -> None:
    value = json_to_value(kind, data)
    assert value_kind(value) == kind
    assert value_to_json(value) == expected


def test_coerce_keeps_kind_and_checks_range() -> None:
    current = json_to_value("natural8", [1, 2])
    assert value_to_json(coerce(current, [3, 255])) == [3, 255]
    assert value_kind(coerce(current, [3.0, 4])) == "natural8"
    with pytest.raises(RegisterError, match="out of range"):
        coerce(current, [3, 256])
    with pytest.raises(RegisterError, match="out of range"):
        coerce(current, [1.5, 2])
    with pytest.raises(RegisterError, match=r"natural8\[2\]"):
        coerce(current, 3)
    with pytest.raises(RegisterError, match="not a number"):
        coerce(current, ["a", 1])
    with pytest.raises(RegisterError, match="empty"):
        coerce(uavcan.register.Value_1_0(), 1)
    assert value_to_json(coerce(json_to_value("real32", [0.0]), 2)) == [2.0]


def test_flatten() -> None:
    data = {"a": 1, "b": {"c": [1.5, 2.5], "d": True}, "s": "text", "long": list(range(100))}
    assert flatten(data) == {"a": 1.0, "b.c[0]": 1.5, "b.c[1]": 2.5, "b.d": 1.0}


def test_cli_parses_redundant_interfaces() -> None:
    args = _parser().parse_args(["serve", "-i", "canable:/dev/ttyACM0", "-i", "canable:/dev/ttyACM1", "--pnp"])
    assert args.iface == ["canable:/dev/ttyACM0", "canable:/dev/ttyACM1"]
    assert args.pnp == "cymon-pnp.db"
    assert args.bitrate == 1_000_000 and args.data_bitrate is None and not args.classic


def test_cli_serve_without_interface() -> None:
    assert _parser().parse_args(["serve"]).iface is None
    assert _parser().parse_args([]).command is None  # main() runs serve --open


def test_version_matches_pyproject() -> None:
    """release-please bumps both; they must not drift apart."""
    pyproject = (Path(__file__).parents[1] / "pyproject.toml").read_text()
    assert re.search(r'^version = "(.+)"$', pyproject, re.MULTILINE)[1] == __version__  # type: ignore[index]
