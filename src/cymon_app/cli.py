# SPDX-License-Identifier: AGPL-3.0-or-later
"""Command line: ``cymon serve``, ``cymon demo``, ``cymon adapters``, ``cymon dsdl``.

``cymon`` alone is ``cymon serve --open`` without an interface: the web UI
opens in the browser and connects from there. That is also what a double click
on the executable does.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import os
import sys
import traceback
import webbrowser
from typing import TYPE_CHECKING

from . import __version__, dsdl
from .transport import find_adapters

if TYPE_CHECKING:
    from .session import BusConfig

_logger = logging.getLogger("cymon")


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
    parser = argparse.ArgumentParser(
        prog="cymon", description="Cyphal admin tool with a web UI. Without a command: serve --open."
    )
    parser.add_argument("--version", action="version", version=f"cymon {__version__}")
    parser.add_argument("-v", "--verbose", action="count", default=0)
    sub = parser.add_subparsers(dest="command")

    serve = sub.add_parser("serve", help="serve the web UI and connect to the bus (or choose the bus in the UI)")
    serve.add_argument(
        "-i",
        "--iface",
        action="append",
        metavar="KIND:CHANNEL",
        help="canable:/dev/ttyACM0, canable:COM5, socketcan:can0, pcan:PCAN_USBBUS1, udp:127.0.0.1 ... "
        "(repeat for a redundant bus; omit to choose in the web UI)",
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

    demo = sub.add_parser("demo", help="simulated devices on a virtual bus, no hardware needed")
    demo.add_argument("--devices", type=int, default=1, help="number of simulated devices (node-IDs 10, 11, ...)")
    _add_common(demo)

    sub.add_parser("adapters", help="list CAN adapters connected to this computer")

    compile_cmd = sub.add_parser("dsdl", help="compile the DSDL and print the package directory")
    compile_cmd.add_argument("--dsdl", action="append", metavar="DIR")
    return parser


async def _serve(args: argparse.Namespace, config: BusConfig | None) -> None:
    import pycyphal.transport
    import uvicorn

    from .session import Session
    from .web.server import create_app

    session = Session(dsdl_roots=_dsdl_roots(args))
    if config is not None:
        try:
            await session.connect(config)
        except (OSError, ValueError, pycyphal.transport.InvalidMediaConfigurationError) as ex:
            raise SystemExit(f"cymon: cannot connect to {', '.join(config.interfaces)}: {ex}") from ex
    url = f"http://{'localhost' if args.host in ('127.0.0.1', '0.0.0.0') else args.host}:{args.port}"
    _logger.info("Web UI: %s%s", url, "" if config else " (choose the bus there)")
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        _logger.warning("The web UI on %s lets anyone on that network command the bus.", args.host)
    if args.open:
        webbrowser.open(url)
    web = uvicorn.Config(create_app(session), host=args.host, port=args.port, log_level="warning")
    try:
        await uvicorn.Server(web).serve()
    finally:
        session.close()


def _cmd_serve(args: argparse.Namespace) -> None:
    dsdl.setup(_dsdl_roots(args))
    from .session import BusConfig

    config = None
    if args.iface:
        config = BusConfig(args.iface, args.node_id, args.bitrate, args.data_bitrate, args.classic, args.pnp)
    asyncio.run(_serve(args, config))


def _cmd_demo(args: argparse.Namespace) -> None:
    dsdl.setup(_dsdl_roots(args))
    from .session import BusConfig

    asyncio.run(_serve(args, BusConfig(demo_devices=args.devices)))


def _cmd_adapters(_: argparse.Namespace) -> None:
    adapters = find_adapters()
    for adapter in adapters:
        print(f"{adapter['device']:20} {adapter['description']:50} -i {adapter['interface']}")
    if not adapters:
        print("No CAN adapter found. A CANable 2.0 shows up as a serial port (slcan firmware) or as a SocketCAN")
        print("interface (candleLight firmware, Linux). See the README, section 'CAN adapters'.")


def _cmd_dsdl(args: argparse.Namespace) -> None:
    print(dsdl.setup(_dsdl_roots(args)))


def main(argv: list[str] | None = None) -> None:
    argv = sys.argv[1:] if argv is None else argv
    parser = _parser()
    args = parser.parse_args(argv)
    if args.command is None:
        args = parser.parse_args([*argv, "serve", "--open"])
    level = logging.WARNING - 10 * min(args.verbose + 1, 2)
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s", stream=sys.stderr)
    if args.verbose < 2:
        for name in ("pycyphal", "pydsdl", "nunavut", "can"):
            logging.getLogger(name).setLevel(logging.WARNING)
    commands = {"serve": _cmd_serve, "demo": _cmd_demo, "adapters": _cmd_adapters, "dsdl": _cmd_dsdl}
    try:
        with contextlib.suppress(KeyboardInterrupt):
            commands[args.command](args)
    except Exception:
        if argv or not getattr(sys, "frozen", False):
            raise
        # Started by a double click on the executable: keep the console window open to show the error.
        traceback.print_exc()
        input("Press Enter to close.")


if __name__ == "__main__":
    main()
