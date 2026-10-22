# SPDX-License-Identifier: AGPL-3.0-or-later
"""Builds the pycyphal transport from interface specifications.

An interface is ``<kind>:<channel>``:

====================== ================================================================
``canable:<port>``     CANable 2.0 (Pro) with the slcan firmware (``/dev/ttyACM0``, ``COM5``)
``socketcan:<iface>``  Linux SocketCAN, e.g. a CANable with candleLight firmware or ``vcan0``
``<python-can>:<ch>``  Any python-can interface pycyphal supports (``pcan:PCAN_USBBUS1``,
                       ``gs_usb:0``, ``slcan:/dev/ttyACM0`` for Classic CAN, ``virtual:bus``)
``udp:<ip>``           Cyphal/UDP on the local interface ``ip`` (simulation)
====================== ================================================================

Several CAN interfaces form one redundant Cyphal transport: every transfer
goes out on all of them and is deduplicated on reception.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

import pycyphal.transport
from pycyphal.transport.can import CANTransport
from pycyphal.transport.redundant import RedundantTransport

from . import canable

#: USB IDs of serial-port CAN adapters cymon knows how to drive. A candleLight adapter is no serial port;
#: on Linux it shows up as a SocketCAN interface instead.
SERIAL_ADAPTERS = {
    (0x16D0, 0x117E): ("CANable 2.0 (slcan firmware)", "canable:{port}"),
}


@dataclasses.dataclass(frozen=True)
class CanSettings:
    bitrate: int = 1_000_000
    #: Data phase rate; None or equal to ``bitrate`` sends CAN FD frames without bit rate switch.
    data_bitrate: int | None = None
    #: 64 for CAN FD (default), 8 for Classic CAN.
    mtu: int = 64


def make_media(spec: str, settings: CanSettings) -> pycyphal.transport.can.media.Media:
    kind, sep, channel = spec.partition(":")
    if not sep:
        raise ValueError(f"Interface {spec!r} is not <kind>:<channel>")
    if kind == "socketcan":
        from pycyphal.transport.can.media.socketcan import SocketCANMedia

        return SocketCANMedia(channel, mtu=settings.mtu)

    from pycyphal.transport.can.media.pythoncan import PythonCANMedia

    if kind == "canable":
        canable.register()
    bitrate: int | tuple[int, int]
    bitrate = (settings.bitrate, settings.data_bitrate or settings.bitrate) if settings.mtu > 8 else settings.bitrate
    return PythonCANMedia(spec, bitrate, settings.mtu)


def make_transport(
    interfaces: list[str], node_id: int | None, settings: CanSettings | None = None
) -> pycyphal.transport.Transport:
    """One transport for all interfaces; several of them become a redundant group."""
    if not interfaces:
        raise ValueError("No interface given (e.g. canable:/dev/ttyACM0, socketcan:can0, udp:127.0.0.1)")
    settings = settings or CanSettings()
    transports: list[pycyphal.transport.Transport] = []
    for spec in interfaces:
        if spec.startswith("udp:"):
            from pycyphal.transport.udp import UDPTransport

            transports.append(UDPTransport(spec.partition(":")[2], local_node_id=node_id))
        else:
            transports.append(CANTransport(make_media(spec, settings), local_node_id=node_id))
    if len(transports) == 1:
        return transports[0]
    redundant = RedundantTransport()
    for t in transports:
        redundant.attach_inferior(t)
    return redundant


def describe(interfaces: list[str], settings: CanSettings) -> str:
    rate = f"{settings.bitrate // 1000} kbit/s"
    if settings.mtu > 8:
        data = settings.data_bitrate or settings.bitrate
        rate += f", CAN FD MTU {settings.mtu}" + (
            f", data {data // 1000} kbit/s (BRS)" if data != settings.bitrate else ", no BRS"
        )
    return f"{' + '.join(interfaces)} ({rate})"


def find_adapters() -> list[dict[str, Any]]:
    """CAN adapters connected to this computer: USB serial adapters and SocketCAN interfaces (Linux)."""
    from serial.tools import list_ports

    found: list[dict[str, Any]] = []
    for port in list_ports.comports():
        known = SERIAL_ADAPTERS.get((port.vid or 0, port.pid or 0))
        if known:
            found.append(
                {"device": port.device, "description": known[0], "interface": known[1].format(port=port.device)}
            )
    net = Path("/sys/class/net")
    if net.is_dir():
        for path in sorted(net.iterdir()):
            try:
                if (path / "type").read_text().strip() != "280":  # ARPHRD_CAN
                    continue
                state = (path / "operstate").read_text().strip()
                fd = "CAN FD" if (path / "mtu").read_text().strip() == "72" else "Classic CAN"
            except OSError:
                continue
            found.append(
                {"device": path.name, "description": f"SocketCAN, {state}, {fd}", "interface": f"socketcan:{path.name}"}
            )
    return found
