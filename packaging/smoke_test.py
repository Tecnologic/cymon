# SPDX-License-Identifier: AGPL-3.0-or-later
"""Smoke test of a built executable: python packaging/smoke_test.py dist/cymon

Starts it without a bus, connects to the simulated devices through the API
like the web UI does, reads registers and disconnects. Standard library only.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

PORT = 8799
BASE = f"http://127.0.0.1:{PORT}"


def call(method: str, path: str, body: object = None) -> object:
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(BASE + path, data, {"Content-Type": "application/json"}, method=method)
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def wait_for(what: str, check: object, timeout: float = 120.0) -> object:
    deadline = time.monotonic() + timeout
    while True:
        try:
            result = check()  # type: ignore[operator]
            if result:
                return result
        except (urllib.error.URLError, ConnectionError):
            pass
        if time.monotonic() > deadline:
            raise TimeoutError(what)
        time.sleep(0.5)


def main() -> None:
    exe = sys.argv[1]
    print(subprocess.run([exe, "--version"], check=True, capture_output=True, text=True).stdout.strip())
    server = subprocess.Popen([exe, "serve", "--port", str(PORT)])
    try:
        status = wait_for("web UI", lambda: call("GET", "/api/status"))
        assert not status["connected"], status
        assert "cymon" in status["dsdl"]["namespaces"], status
        assert "uavcan.node.Heartbeat.1.0" in call("GET", "/api/types")
        assert b"cymon" in urllib.request.urlopen(BASE + "/", timeout=10).read()

        assert call("POST", "/api/connect", {"demo_devices": 1})["connected"]
        wait_for("demo node", lambda: any(n["info"] for n in call("GET", "/api/nodes")))
        names = [r["name"] for r in call("GET", "/api/nodes/10/registers")]
        assert "demo.frequency" in names, names
        assert not call("POST", "/api/disconnect")["connected"]
        print("smoke test passed")
    finally:
        server.terminate()
        server.wait(10)


if __name__ == "__main__":
    main()
