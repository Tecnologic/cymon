# SPDX-License-Identifier: AGPL-3.0-or-later
"""cymon captures through the scope client against the simulated cymon-lib device."""

from __future__ import annotations

import pytest

from cymon_app.admin import Admin
from cymon_app.demo import DemoDevice
from cymon_app.scope import CaptureRequest, ScopeError
from cymon_app.transport import make_transport

from .conftest import DEVICE_ID, wait_for


async def test_info(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    info = await admin.scope.info(DEVICE_ID)
    assert info["services"] == {"variables": 120, "buffer_info": 121, "setup": 122, "arm": 123, "read": 124}
    assert [v["name"] for v in info["variables"]] == ["sine", "cosine", "ramp", "engaged"]
    assert info["variables"][0]["unit"] == "A"
    assert info["buffer"] == {
        "buffer_bytes": 4096,
        "max_channels": 8,
        "max_frames": 1024,
        "max_read_samples": 64,
        "tick_period_us": 100.0,
    }


async def test_immediate_capture_pages_all_frames(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    result = await admin.scope.capture(
        DEVICE_ID, CaptureRequest(channels=[0, 1, 2], sample_period_us=250, num_samples=100)
    )
    assert result["sample_period_us"] == 300.0  # 2.5 ticks rounds to 3
    assert len(result["t"]) == 100 and len(result["channels"]) == 3
    assert all(len(c) == 100 for c in result["channels"])
    assert result["t"][0] == 0.0 and result["t"][1] == pytest.approx(300e-6)
    assert (await admin.scope.status(DEVICE_ID))["state"] == "complete"


async def test_rising_trigger_with_pretrigger(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    request = CaptureRequest(
        channels=[0],
        sample_period_us=100,
        num_samples=60,
        pretrigger_samples=20,
        trigger_mode="rising",
        trigger_source=0,
        trigger_level=0.5,
    )
    result = await admin.scope.capture(DEVICE_ID, request)
    sine = result["channels"][0]
    assert result["t"][20] == 0.0
    assert sine[19] < 0.5 <= sine[20]  # the trigger sample is the first one at or above the level


async def test_errors(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    with pytest.raises(ScopeError, match="bad channel id"):
        await admin.scope.capture(DEVICE_ID, CaptureRequest(channels=[9], sample_period_us=100, num_samples=10))
    with pytest.raises(ScopeError, match="buffer too small"):
        await admin.scope.capture(DEVICE_ID, CaptureRequest(channels=[0, 1], sample_period_us=100, num_samples=600))
    with pytest.raises(ScopeError, match="bad config"):
        await admin.scope.capture(
            DEVICE_ID, CaptureRequest(channels=[0], sample_period_us=100, num_samples=10, pretrigger_samples=10)
        )
    with pytest.raises(ScopeError, match="Unknown trigger mode"):
        await admin.scope.capture(
            DEVICE_ID, CaptureRequest(channels=[0], sample_period_us=100, num_samples=10, trigger_mode="x")
        )


async def test_timeout_disarms(bus: tuple[Admin, DemoDevice]) -> None:
    admin, _ = bus
    request = CaptureRequest(
        channels=[0], sample_period_us=100, num_samples=10, trigger_mode="rising", trigger_level=100.0, timeout_s=0.5
    )
    with pytest.raises(ScopeError, match="did not complete"):
        await admin.scope.capture(DEVICE_ID, request)
    assert (await admin.scope.status(DEVICE_ID))["state"] == "configured"


async def test_node_without_cymon(bus: tuple[Admin, DemoDevice], channel: str) -> None:
    admin, _ = bus
    other = Admin(make_transport([channel], 50), unique_id=bytes([1] * 16))  # registers, no cymon services
    other.start()
    try:
        await wait_for(lambda: 50 in admin.nodes)
        with pytest.raises(ScopeError, match="no cymon services"):
            await admin.scope.info(50)
        with pytest.raises(ScopeError, match="Cannot read the port registers"):
            await admin.scope.info(99)
    finally:
        other.close()
