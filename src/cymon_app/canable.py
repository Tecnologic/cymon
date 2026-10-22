# SPDX-License-Identifier: AGPL-3.0-or-later
"""CANable 2.0 (Pro) with the slcan firmware, as a CAN FD interface for pycyphal.

The CANable 2.0 slcan firmware (normaldotcom/canable2-fw) extends the LAWICEL
protocol with CAN FD: ``d``/``D`` send FD frames without bit rate switch at
the nominal rate, ``b``/``B`` with bit rate switch at the data rate, and
``Y2``/``Y5`` set the data rate to 2 or 5 Mbit/s. python-can's ``slcan``
interface speaks this, but pycyphal only uses it for Classic CAN and always
sets the bit rate switch on FD frames.

:class:`CanableBus` fixes both: it is a python-can bus (interface name
``canable``) that always opens in FD mode and sends with bit rate switch only
when a data rate other than the nominal one was configured. :func:`register`
makes ``canable:<port>`` usable as a pycyphal ``PythonCANMedia`` interface.

With the candleLight (gs_usb) firmware on Linux the adapter is a SocketCAN
interface instead and needs none of this (``socketcan:can0``).
"""

from __future__ import annotations

import logging
from typing import Any

import can
from can.interfaces.slcan import slcanBus

_logger = logging.getLogger(__name__)

#: Data rates the CANable 2.0 firmware can set (Y2, Y5).
DATA_BITRATES = (2_000_000, 5_000_000)
#: Nominal rates of the LAWICEL S command.
NOMINAL_BITRATES = tuple(sorted(slcanBus._BITRATES))


class CanableBus(slcanBus):
    """slcan bus of a CANable 2.0 in CAN FD mode.

    :param channel: Serial port (``/dev/ttyACM0``, ``COM5``), optionally ``@<baud>``.
    :param bitrate: Nominal (arbitration) bit rate.
    :param data_bitrate: Data phase bit rate. ``None`` or equal to ``bitrate``: FD frames
        without bit rate switch (for transceivers or buses that stay at the nominal rate,
        such as a UNIMOC BCC with its classic CAN transceiver). 2 or 5 Mbit/s: frames with
        bit rate switch.
    """

    def __init__(
        self,
        channel: str,
        bitrate: int = 1_000_000,
        data_bitrate: int | None = None,
        sleep_after_open: float = 0.5,
        **kwargs: Any,
    ) -> None:
        if bitrate not in slcanBus._BITRATES:
            raise ValueError(f"CANable: nominal bit rate {bitrate} not supported, use one of {NOMINAL_BITRATES}")
        brs = data_bitrate is not None and data_bitrate != bitrate
        if brs and data_bitrate not in DATA_BITRATES:
            raise ValueError(f"CANable: data bit rate {data_bitrate} not supported, use one of {DATA_BITRATES}")
        kwargs.pop("fd", None)  # always FD
        super().__init__(channel, bitrate=None, sleep_after_open=sleep_after_open, **kwargs)
        self.set_bitrate(bitrate, data_bitrate if brs else None)
        self._bitrate_switch = brs
        self._can_protocol = can.CanProtocol.CAN_FD
        _logger.info(
            "CANable on %s: %d bit/s nominal, %s", channel, bitrate, f"{data_bitrate} bit/s data" if brs else "no BRS"
        )

    def send(self, msg: can.Message, timeout: float | None = None) -> None:
        if msg.is_fd:
            msg.bitrate_switch = self._bitrate_switch
        super().send(msg, timeout)


_registered = False


def register() -> None:
    """Makes ``canable:<port>`` available to pycyphal's PythonCANMedia.

    python-can itself finds :class:`CanableBus` through the ``can.interface``
    entry point of this package (``interface="canable"``), so cymon has to be
    installed (``pip install -e .`` is enough).
    """
    global _registered
    if _registered:
        return
    if "canable" not in can.VALID_INTERFACES:
        raise RuntimeError("python-can does not know the 'canable' interface: install cymon (pip install -e .)")

    # pycyphal selects the python-can bus constructor by interface name; its
    # table is private, so this is the one place that depends on its layout.
    from pycyphal.transport.can.media.pythoncan import _pythoncan

    def construct(params: Any) -> tuple[Any, can.ThreadSafeBus]:
        if isinstance(params, _pythoncan._FDInterfaceParameters):
            nominal, data = params.bitrate
        else:
            nominal, data = params.bitrate, None
        bus = can.ThreadSafeBus(interface="canable", channel=params.channel_name, bitrate=nominal, data_bitrate=data)
        return _pythoncan.PythonCANBusOptions(), bus

    _pythoncan._CONSTRUCTORS["canable"] = construct
    _registered = True
