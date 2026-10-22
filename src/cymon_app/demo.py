# SPDX-License-Identifier: AGPL-3.0-or-later
"""Simulated device node for trying the tool without hardware and for the tests.

It behaves like a small drive with cymon-lib:

- registers ``demo.*`` (amplitude, frequency, read-only counters) and the
  cymon service port registers ``uavcan.srv.cymon_*.{id,type}``;
- the five cymon services, with the capture logic of cymon-lib
  (decimation, pre-trigger, edge/hysteresis/immediate trigger) on a virtual
  tick of ``tick_period_us``;
- ``uavcan.primitive.array.Real32.1.0`` telemetry [0.5 Hz sine, cosine, i_q]
  on subject 1000 at 20 Hz;
- a readiness subscription (``reg.udral.service.common.Readiness.0.1`` on
  subject 106) that engages only while refreshed within 0.5 s, like UNIMOC;
- ExecuteCommand (restart, store, emergency stop) and diagnostic records.

Import only after :func:`cymon_app.dsdl.setup`.
"""

from __future__ import annotations

import asyncio
import hashlib
import math
import time
from typing import Any

import cymon as cymon_dsdl
import pycyphal.application
import pycyphal.transport
import reg.udral.service.common
import uavcan.diagnostic
import uavcan.node
import uavcan.primitive.array
from pycyphal.application.register import Natural16, Real32, Value

SERVICE_IDS = {
    "cymon_variables": 120,
    "cymon_buffer_info": 121,
    "cymon_setup": 122,
    "cymon_arm": 123,
    "cymon_read": 124,
}
TELEMETRY_SUBJECT = 1000
READINESS_SUBJECT = 106
ENGAGED = 3

IDLE, CONFIGURED, ARMED, TRIGGERED, COMPLETE = range(5)
RISING, FALLING, HYSTERESIS, IMMEDIATE = range(4)


class CaptureEngine:
    """Python port of cymon::Device (capture part), driven by Tick()."""

    def __init__(
        self,
        variables: list[tuple[str, str]],
        read: Any,
        tick_period_us: float,
        buffer_floats: int = 1024,
        max_channels: int = 8,
    ) -> None:
        self.variables = variables
        self.read = read
        self.tick_period_us = tick_period_us
        self.buffer_floats = buffer_floats
        self.max_channels = max_channels
        self.state = IDLE
        self.channels: list[int] = []
        self.num_samples = 0
        self.pretrigger = 0
        self.decimation = 1
        self.period_us = 0.0
        self.counter = 0
        self.pre: list[list[float]] = []
        self.post: list[list[float]] = []
        self.mode = RISING
        self.source = 0
        self.level = 0.0
        self.band = 0.0
        self.prev = 0.0
        self.awaiting_low = False
        self.trigger_requested = False

    def setup(self, channels: list[int], period_us: float, num_samples: int, pretrigger: int) -> tuple[int, float]:
        if self.state in (ARMED, TRIGGERED):
            return 5, 0.0
        if not channels or len(channels) > self.max_channels or num_samples == 0 or pretrigger >= num_samples:
            return 9, 0.0
        if any(c >= len(self.variables) for c in channels):
            return 1, 0.0
        if num_samples > self.buffer_floats // len(channels):
            return 2, 0.0
        self.channels, self.num_samples, self.pretrigger = list(channels), num_samples, pretrigger
        self.decimation = (
            max(1, min(65535, int(period_us / self.tick_period_us + 0.5))) if period_us > self.tick_period_us else 1
        )
        self.period_us = self.decimation * self.tick_period_us
        self.state = CONFIGURED
        return 0, self.period_us

    def arm(self, arm: bool, source: int, mode: int, level: float, band: float) -> int:
        if self.state == IDLE:
            return 3 if arm else 0
        self.state = CONFIGURED
        if not arm:
            return 0
        if mode > IMMEDIATE:
            return 9
        if mode != IMMEDIATE and source >= len(self.variables):
            return 1
        self.mode, self.source, self.level, self.band = mode, source, level, band
        self.prev = self.read(source) if source < len(self.variables) else 0.0
        self.awaiting_low = mode == HYSTERESIS and self.prev >= level
        self.pre, self.post, self.counter, self.trigger_requested = [], [], 0, False
        self.state = ARMED
        return 0

    def _fires(self, value: float) -> bool:
        if self.mode == IMMEDIATE:
            return True
        if self.mode == RISING:
            return self.prev < self.level <= value
        if self.mode == FALLING:
            return self.prev > self.level >= value
        if self.awaiting_low:
            if value < self.level - self.band:
                self.awaiting_low = False
            return False
        return self.prev < self.level <= value

    def tick(self) -> None:
        if self.state not in (ARMED, TRIGGERED):
            return
        self.counter += 1
        if self.counter < self.decimation:
            return
        self.counter = 0
        if self.state == ARMED:
            value = self.read(self.source) if self.source < len(self.variables) else 0.0
            filled = len(self.pre) >= self.pretrigger
            if filled and (self._fires(value) or self.trigger_requested):
                self.state = TRIGGERED
            self.prev = value
        frame = [self.read(c) for c in self.channels]
        if self.state == ARMED:
            self.pre.append(frame)
            if len(self.pre) > self.pretrigger:
                self.pre.pop(0)
        else:
            self.post.append(frame)
            if len(self.post) >= self.num_samples - self.pretrigger:
                self.state = COMPLETE

    def frames(self) -> list[list[float]]:
        missing = self.pretrigger - len(self.pre)
        return [[0.0] * len(self.channels)] * missing + self.pre + self.post


class DemoDevice:
    def __init__(
        self,
        transport: pycyphal.transport.Transport,
        name: str = "org.tecnologic.cymon.demo",
        tick_period_us: float = 100.0,
    ) -> None:
        env = {f"UAVCAN__SRV__{k.upper()}__ID": str(v) for k, v in SERVICE_IDS.items()}
        env["UAVCAN__PUB__TELEMETRY__ID"] = str(TELEMETRY_SUBJECT)
        env["UAVCAN__SUB__READINESS__ID"] = str(READINESS_SUBJECT)
        registry = pycyphal.application.make_registry(None, env)
        info = uavcan.node.GetInfo_1.Response(
            protocol_version=uavcan.node.Version_1(1, 0),
            software_version=uavcan.node.Version_1(0, 2),
            hardware_version=uavcan.node.Version_1(1, 0),
            unique_id=hashlib.md5(name.encode()).digest(),
            name=name,
        )
        self.node = pycyphal.application.make_node(info, registry, transport=transport)
        self.node.heartbeat_publisher.mode = uavcan.node.Mode_1.OPERATIONAL
        self.amplitude = 1.0
        self.frequency = 50.0
        self.engaged = False
        self.estopped = False
        self.readiness_deadline = 0.0
        self.store_count = 0
        self.t = 0.0
        registers = self.node.registry
        registers["demo.amplitude"] = lambda: Value(real32=Real32([self.amplitude])), self._set_amplitude
        registers["demo.frequency"] = lambda: Value(real32=Real32([self.frequency])), self._set_frequency
        registers["demo.engaged"] = lambda: Value(natural16=Natural16([int(self.engaged)]))
        registers["demo.store_count"] = lambda: Value(natural16=Natural16([self.store_count]))

        variables = [("sine", "A"), ("cosine", "A"), ("ramp", ""), ("engaged", "")]
        self.capture = CaptureEngine(variables, self._read_variable, tick_period_us)
        self._tick_period_s = tick_period_us * 1e-6
        self._tasks: list[asyncio.Task[None]] = []

        get = self.node.get_server
        get(cymon_dsdl.GetVariableList_0_1, "cymon_variables").serve_in_background(self._on_variables)
        get(cymon_dsdl.GetBufferInfo_0_1, "cymon_buffer_info").serve_in_background(self._on_buffer_info)
        get(cymon_dsdl.SetupCapture_0_1, "cymon_setup").serve_in_background(self._on_setup)
        get(cymon_dsdl.ArmTrigger_0_1, "cymon_arm").serve_in_background(self._on_arm)
        get(cymon_dsdl.ReadSamples_0_1, "cymon_read").serve_in_background(self._on_read)
        get(uavcan.node.ExecuteCommand_1).serve_in_background(self._on_command)
        self._telemetry = self.node.make_publisher(uavcan.primitive.array.Real32_1_0, "telemetry")
        self._readiness = self.node.make_subscriber(reg.udral.service.common.Readiness_0_1, "readiness")
        self._readiness.receive_in_background(self._on_readiness)
        self._log = self.node.make_publisher(uavcan.diagnostic.Record_1)

    def _set_amplitude(self, value: Value) -> None:
        self.amplitude = float(value.real32.value[0])

    def _set_frequency(self, value: Value) -> None:
        self.frequency = float(value.real32.value[0])

    def _read_variable(self, index: int) -> float:
        w = 2 * math.pi * self.frequency * self.t
        return [self.amplitude * math.sin(w), self.amplitude * math.cos(w), (self.t * 10.0) % 1.0, float(self.engaged)][
            index
        ]

    def start(self) -> None:
        self.node.start()
        loop = asyncio.get_running_loop()
        self._tasks = [loop.create_task(self._sampling()), loop.create_task(self._publishing())]

    def close(self) -> None:
        for task in self._tasks:
            task.cancel()
        self.node.close()

    async def _sampling(self) -> None:
        """Advances the virtual tick in real time (in batches, like an interrupt that catches up)."""
        last = time.monotonic()
        while True:
            await asyncio.sleep(0.005)
            now = time.monotonic()
            ticks = int((now - last) / self._tick_period_s)
            last += ticks * self._tick_period_s
            for _ in range(ticks):
                self.t += self._tick_period_s
                self.capture.tick()

    async def _publishing(self) -> None:
        while True:
            await asyncio.sleep(0.05)
            if self.engaged and time.monotonic() > self.readiness_deadline:
                await self._set_engaged(False, "readiness timeout")
            # Slow signals: the 20 Hz telemetry would alias the 50 Hz scope signal.
            w = 2 * math.pi * 0.5 * self.t
            values = [self.amplitude * math.sin(w), self.amplitude * math.cos(w), 5.0 * float(self.engaged)]
            await self._telemetry.publish(uavcan.primitive.array.Real32_1_0(values))

    async def _set_engaged(self, engaged: bool, why: str) -> None:
        if engaged != self.engaged:
            self.engaged = engaged
            text = f"{'engaged' if engaged else 'disengaged'}: {why}"
            await self._log.publish(
                uavcan.diagnostic.Record_1(severity=uavcan.diagnostic.Severity_1(4 if not engaged else 2), text=text)
            )

    async def _on_readiness(self, msg: Any, _: pycyphal.transport.TransferFrom) -> None:
        if msg.value == ENGAGED and not self.estopped:
            self.readiness_deadline = time.monotonic() + 0.5
            await self._set_engaged(True, "readiness ENGAGED")
        else:
            await self._set_engaged(False, "readiness not ENGAGED")

    async def _on_command(self, request: Any, _: Any) -> Any:
        Status = uavcan.node.ExecuteCommand_1.Response
        if request.command == 65531:
            self.estopped = True
            await self._set_engaged(False, "emergency stop")
            return Status(status=Status.STATUS_SUCCESS)
        if request.command == 65530:
            if self.engaged:
                return Status(status=Status.STATUS_BAD_STATE)
            self.store_count += 1
            return Status(status=Status.STATUS_SUCCESS)
        if request.command == 65535:
            self.estopped = False
            return Status(status=Status.STATUS_SUCCESS)
        return Status(status=Status.STATUS_BAD_COMMAND)

    async def _on_variables(self, request: Any, _: Any) -> Any:
        if request.index < len(self.capture.variables):
            name, unit = self.capture.variables[request.index]
            return cymon_dsdl.GetVariableList_0_1.Response(name=name, unit=unit)
        return cymon_dsdl.GetVariableList_0_1.Response()

    async def _on_buffer_info(self, _: Any, __: Any) -> Any:
        c = self.capture
        return cymon_dsdl.GetBufferInfo_0_1.Response(
            buffer_bytes=4 * c.buffer_floats,
            max_channels=c.max_channels,
            max_frames=c.buffer_floats,
            max_read_samples=64,
            tick_period_us=c.tick_period_us,
        )

    async def _on_setup(self, request: Any, _: Any) -> Any:
        code, period = self.capture.setup(
            [int(c) for c in request.channel_ids],
            float(request.sample_period_us),
            int(request.num_samples),
            int(request.pretrigger_samples),
        )
        return cymon_dsdl.SetupCapture_0_1.Response(result=cymon_dsdl.Error_0_1(code), actual_sample_period_us=period)

    async def _on_arm(self, request: Any, _: Any) -> Any:
        code = self.capture.arm(
            bool(request.arm),
            int(request.source_variable_id),
            int(request.mode.value),
            float(request.level),
            float(request.hysteresis_band),
        )
        return cymon_dsdl.ArmTrigger_0_1.Response(result=cymon_dsdl.Error_0_1(code))

    async def _on_read(self, request: Any, _: Any) -> Any:
        c = self.capture
        resp = cymon_dsdl.ReadSamples_0_1.Response(state=cymon_dsdl.CaptureState_0_1(c.state))
        if c.state == IDLE:
            return resp
        resp.num_channels, resp.num_samples, resp.pretrigger_samples, resp.sample_period_us = (
            len(c.channels),
            c.num_samples,
            c.pretrigger,
            c.period_us,
        )
        if c.state != COMPLETE:
            return resp
        per_response = 64 // len(c.channels)
        count = min(int(request.max_frames), per_response)
        frames = c.frames()[request.frame_offset : request.frame_offset + count]
        resp.samples = [v for f in frames for v in f]
        resp.end_of_data = request.frame_offset + len(frames) >= c.num_samples
        return resp
