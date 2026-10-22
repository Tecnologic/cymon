# SPDX-License-Identifier: AGPL-3.0-or-later
"""CANable slcan bus: FD frames with and without bit rate switch, rate checks, pycyphal media."""

from __future__ import annotations

import can
import pytest

from cymon_app import canable
from cymon_app.transport import CanSettings, describe, make_media, make_transport


def _bus(**kwargs: object) -> canable.CanableBus:
    return can.Bus(interface="canable", channel="loop://", sleep_after_open=0, **kwargs)  # type: ignore[return-value]


def _sent(bus: canable.CanableBus, msg: can.Message) -> list[str]:
    written: list[str] = []
    bus._write = written.append  # type: ignore[method-assign]
    bus.send(msg)
    return written


def test_fd_without_brs_at_nominal_rate() -> None:
    bus = _bus(bitrate=1_000_000, data_bitrate=1_000_000)
    msg = can.Message(arbitration_id=0x10ABCDEF, is_extended_id=True, is_fd=True, bitrate_switch=True, data=bytes(12))
    assert _sent(bus, msg) == ["D10ABCDEF9" + "00" * 12]
    bus.shutdown()


def test_fd_with_brs_at_data_rate() -> None:
    bus = _bus(bitrate=500_000, data_bitrate=2_000_000)
    msg = can.Message(arbitration_id=0x1, is_extended_id=True, is_fd=True, data=b"\x01")
    assert _sent(bus, msg) == ["B00000001101"]
    bus.shutdown()


def test_classic_frame_passes_through() -> None:
    bus = _bus(bitrate=1_000_000)
    msg = can.Message(arbitration_id=0x123, is_extended_id=True, data=b"\xaa")
    assert _sent(bus, msg) == ["T000001231AA"]
    bus.shutdown()


def test_unsupported_rates_are_rejected() -> None:
    with pytest.raises(ValueError, match="nominal"):
        _bus(bitrate=1_234_000)
    with pytest.raises(ValueError, match="data bit rate"):
        _bus(bitrate=1_000_000, data_bitrate=4_000_000)


async def test_pycyphal_media_and_redundant_transport() -> None:
    media = make_media("canable:loop://", CanSettings())
    assert media.mtu == 64
    media.close()
    transport = make_transport(["virtual:a", "virtual:b"], 5)
    assert type(transport).__name__ == "RedundantTransport"
    assert len(transport.inferiors) == 2
    transport.close()
    with pytest.raises(ValueError, match="kind"):
        make_transport(["can0"], 5)


def test_describe() -> None:
    assert "no BRS" in describe(["canable:COM5"], CanSettings())
    assert "data 2000 kbit/s (BRS)" in describe(["canable:COM5"], CanSettings(500_000, 2_000_000))
    assert "FD" not in describe(["slcan:COM5"], CanSettings(mtu=8))
