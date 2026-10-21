# SPDX-License-Identifier: AGPL-3.0-or-later
"""Command line: ``cymon serve``, ``cymon demo``, ``cymon adapters``, ``cymon dsdl``."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import os
import sys
import webbrowser
from pathlib import Path

from . import __version__, dsdl
from .transport import CanSettings, describe, make_transport

_logger = logging.getLogger("cymon")

#: USB IDs of CAN adapters cymon knows how to drive.
_USB_ADAPTERS = {
    (0x16D0, 0x117E): ("CANable 2.0 (slcan firmware)", "canable:{port}"),
    (0x1D50, 0x606F): ("candleLight / gs_usb (CANable with candleLight firmware)", "socketcan:canX or gs_usb:{index}"),
}


def _dsdl_roots(args: argparse.Namespace) -> list[str]:
    roots = list(args.dsdl or [])
    roots += [p for p in os.environ.get("CYMON_DSDL", "").split(os.pathsep) if p]
    return roots


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--dsdl",
        action="append",
        metavar="DIR",
        help="extra DSDL root namespace, e.g. unimoc/dsdl/unimoc (repeatable; also $CYMON_DSDL)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="address of the web UI (default 127.0.0.1; 0.0.0.0 exposes bus control to the network)",
    )
    parser.add_argument("--port", type=int, default=8765, help="port of the web UI (default 8765)")
    parser.add_argument("--open", action="store_true", help="open the web UI in the browser")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="cymon", description="Cyphal admin tool with a web UI")
    parser.add_argument("--version", action="version", version=f"cymon {__version__}")
    parser.add_argument("-v", "--verbose", action="count", default=0)
    sub = parser.add_subparsers(dest="command", required=True)

    serve = sub.add_parser("serve", help="connect to the bus and serve the web UI")
    serve.add_argument(
        "-i",
        "--iface",
        action="append",
        required=True,
        metavar="KIND:CHANNEL",
        help="canable:/dev/ttyACM0, canable:COM5, socketcan:can0, pcan:PCAN_USBBUS1, udp:127.0.0.1 ... "
        "(repeat for a redundant bus)",
    )
    serve.add_argument("--node-id", type=int, default=127, help="node-ID of the tool (default 127)")
    serve.add_argument("--bitrate", type=int, default=1_000_000, help="nominal CAN bit rate (default 1000000)")
    serve.add_argument(
        "--data-bitrate",
        type=int,
        default=None,
        help="CAN FD data rate; omit for FD frames without bit rate switch (CANable: 2000000 or 5000000)",
    )
    serve.add_argument("--classic", action="store_true", help="Classic CAN (MTU 8) instead of CAN FD")
    serve.add_argument(
        "--pnp",
        nargs="?",
        const="cymon-pnp.db",
        default=None,
        metavar="DB",
        help="run the plug-and-play node-ID allocator with this database (default cymon-pnp.db)",
    )
    _add_common(serve)

    demo = sub.add_parser("demo", help="simulated device on a virtual bus, no hardware needed")
    demo.add_argument("--devices", type=int, default=1, help="number of simulated devices (node-IDs 10, 11, ...)")
    _add_common(demo)

    sub.add_parser("adapters", help="list CAN adapters connected to this computer")

    compile_cmd = sub.add_parser("dsdl", help="compile the DSDL and print the package directory")
    compile_cmd.add_argument("--dsdl", action="append", metavar="DIR")
    return parser


async def _serve(args: argparse.Namespace, setup_bus: object) -> None:
    import uvicorn

    from .web.server import create_app

    admin, extras = setup_bus()  # type: ignore[operator]
    admin.start()
    for extra in extras:
        extra.start()
    url = f"http://{'localhost' if args.host in ('127.0.0.1', '0.0.0.0') else args.host}:{args.port}"
    _logger.info("Node-ID %s on %s", admin.node_id, admin.status()["transport"])
    _logger.info("Web UI: %s", url)
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        _logger.warning("The web UI on %s lets anyone on that network command the bus.", args.host)
    if args.open:
        webbrowser.open(url)
    config = uvicorn.Config(create_app(admin), host=args.host, port=args.port, log_level="warning")
    try:
        await uvicorn.Server(config).serve()
    finally:
        for extra in extras:
            extra.close()
        admin.close()


def _cmd_serve(args: argparse.Namespace) -> None:
    dsdl.setup(_dsdl_roots(args))
    settings = CanSettings(args.bitrate, args.data_bitrate, 8 if args.classic else 64)

    def setup_bus() -> tuple[object, list[object]]:
        from .admin import Admin

        _logger.info("Bus: %s", describe(args.iface, settings))
        transport = make_transport(args.iface, args.node_id, settings)
        return Admin(transport, pnp_database=args.pnp), []

    asyncio.run(_serve(args, setup_bus))


def _cmd_demo(args: argparse.Namespace) -> None:
    dsdl.setup(_dsdl_roots(args))

    def setup_bus() -> tuple[object, list[object]]:
        from .admin import Admin
        from .demo import DemoDevice

        devices = [
            DemoDevice(make_transport(["virtual:cymon-demo"], 10 + i), name=f"org.tecnologic.cymon.demo{i}")
            for i in range(args.devices)
        ]
        return Admin(make_transport(["virtual:cymon-demo"], 127)), devices

    asyncio.run(_serve(args, setup_bus))


def _cmd_adapters(_: argparse.Namespace) -> None:
    found = False
    try:
        from serial.tools import list_ports

        gs_index = 0
        for port in list_ports.comports():
            known = _USB_ADAPTERS.get((port.vid or 0, port.pid or 0))
            if known:
                found = True
                print(f"{port.device:20} {known[0]:50} -i {known[1].format(port=port.device, index=gs_index)}")
    except ImportError:
        pass
    net = Path("/sys/class/net")
    if net.is_dir():
        for iface in sorted(net.iterdir()):
            if (iface / "type").read_text().strip() == "280":  # ARPHRD_CAN
                found = True
                state = (iface / "operstate").read_text().strip()
                mtu = (iface / "mtu").read_text().strip()
                fd = "CAN FD" if mtu == "72" else "Classic CAN"
                print(f"{iface.name:20} SocketCAN, {state}, {fd:41} -i socketcan:{iface.name}")
    if not found:
        print("No CAN adapter found. A CANable 2.0 shows up as a serial port (slcan firmware) or as a SocketCAN")
        print("interface (candleLight firmware, Linux). See the README, section 'CAN adapters'.")


def _cmd_dsdl(args: argparse.Namespace) -> None:
    print(dsdl.setup(_dsdl_roots(args)))


def main(argv: list[str] | None = None) -> None:
    args = _parser().parse_args(argv)
    level = logging.WARNING - 10 * min(args.verbose + 1, 2)
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s", stream=sys.stderr)
    if args.verbose < 2:
        for name in ("pycyphal", "pydsdl", "nunavut", "can"):
            logging.getLogger(name).setLevel(logging.WARNING)
    commands = {"serve": _cmd_serve, "demo": _cmd_demo, "adapters": _cmd_adapters, "dsdl": _cmd_dsdl}
    with contextlib.suppress(KeyboardInterrupt):
        commands[args.command](args)


if __name__ == "__main__":
    main()
