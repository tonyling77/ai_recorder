#!/usr/bin/env python3
"""Check the flows of the recorder in the simulator.

The simulator runs the real application logic of the desktop build, so the
flows of the interface can be checked without the board and without a person
pressing buttons. That is the cheap way to answer "did the last change
break something else".

The tool starts the simulator, drives the pages and the buttons through the
control channel of the simulator, and writes a picture of every page.

    python3 tools/sim_check.py
    python3 tools/sim_check.py --keep    # leave the simulator running

The pictures of the simulator show the layout but not the text. The font of
the Chinese is a binary that only the board carries, so the desktop build
draws the labels of the interface empty. That is a limit of the simulator,
not a fault of the application: the pictures are for the layout, and the
text is checked on the board.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


PROJECT = Path(__file__).resolve().parent.parent
API_PORT = 8266
BACKEND_PORT = 8684


class Api:
    """The JSON-RPC 2.0 control channel of the simulator."""

    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=10)
        self.next_id = 1

    def call(self, method: str, params: dict | None = None) -> dict:
        request = {
            "jsonrpc": "2.0",
            "id": self.next_id,
            "method": method,
            "params": params or {},
        }
        self.next_id += 1
        body = json.dumps(request).encode("utf-8")
        self.sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return self._read_response()

    def _read_response(self) -> dict:
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self.sock.recv(1)
            if not chunk:
                raise RuntimeError("the control channel closed")
            header += chunk
        length = 0
        for line in header.decode("ascii").splitlines():
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("the control channel closed")
            payload += chunk
        return json.loads(payload.decode("utf-8"))

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def simulator_path() -> Path:
    """Find the simulator of the pinned version of the component."""
    tools = PROJECT.parent.parent / "submodule/esp-mosaico-utils/mosaico-tools/tools/gsp-sim"
    sys.path.insert(0, str(tools))
    try:
        from fetch_gspc import resolve_gsp_root, resolve_sim  # noqa: PLC0415
    except ImportError as error:
        raise SystemExit("the simulator of the workspace cannot be found: %s" % error)
    root = resolve_gsp_root(PROJECT)
    if root is None:
        raise SystemExit("the ESP-GSP component is not installed in this project")
    return resolve_sim(gsp_root=root)


def start(simulator: Path, keep: bool) -> tuple[subprocess.Popen, subprocess.Popen]:
    """Start the simulator and the application backend."""
    bundle = PROJECT / "build-sim/hello_backend_assets/app.gspb"
    api_json = PROJECT / "build-sim/hello_backend_assets/bundle_gsp.api.json"
    backend = PROJECT / "build-sim/hello_backend"
    if not bundle.exists() or not backend.exists():
        raise SystemExit(
            "the desktop build is missing: run\n"
            "  python3 mosaico.py project sim --project . --headless --duration 1"
        )
    sim = subprocess.Popen(
        [
            str(simulator),
            "--bundle", str(bundle),
            "--api-json", str(api_json),
            "--api-listen", "tcp://127.0.0.1:%d" % API_PORT,
            "--backend-listen", "tcp://127.0.0.1:%d" % BACKEND_PORT,
            "--frames", "0",
            "--headless",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    app = subprocess.Popen(
        [str(backend), "--endpoint", "tcp://127.0.0.1:%d" % BACKEND_PORT],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(60):
        try:
            socket.create_connection(("127.0.0.1", API_PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.5)
    else:
        raise SystemExit("the simulator did not come up")
    return sim, app


def check(api: Api, name: str, method: str, params: dict | None = None) -> int:
    try:
        result = api.call(method, params)
    except Exception as error:  # noqa: BLE001 - the report is the point
        print("FAIL %-46s %s" % (name, error))
        return 1
    if "error" in result:
        print("FAIL %-46s %s" % (name, result["error"]))
        return 1
    print("ok   %s" % name)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep", action="store_true", help="leave the simulator running")
    args = parser.parse_args()

    sim = app = None
    failures = 0
    try:
        sim, app = start(simulator_path(), args.keep)
        api = Api("127.0.0.1", API_PORT)

        # The application backend owns the pages while it is connected, so
        # the pages are reached the way a person reaches them: with a drag.
        # The taps are the ones of every page.

        # The recorder page: the buttons and the list of the recordings.
        for name in ("btn_record", "btn_upload", "btn_files", "btn_setup"):
            failures += check(api, "tap %s on the recorder page" % name, "tap_object",
                              {"name": name, "scene": 0})

        # The notes page: reached with a drag to the left.
        failures += check(api, "drag to the notes page", "drag", {"x1": 400, "y1": 240, "x2": 60, "y2": 240})
        failures += check(api, "tap the first minute", "tap_object", {"name": "note_row_0", "scene": 1})

        # The settings page: two more drags to the left.
        failures += check(api, "drag to the settings page", "drag", {"x1": 400, "y1": 240, "x2": 60, "y2": 240})
        failures += check(api, "drag to the settings page again", "drag",
                          {"x1": 400, "y1": 240, "x2": 60, "y2": 240})
        for name in ("btn_channel_private", "btn_channel_xiaozhi", "inbox_row_0"):
            failures += check(api, "tap %s on the settings page" % name, "tap_object",
                              {"name": name, "scene": 2})

        # A picture of every page, so the layout can be looked at.
        for scene, name in ((0, "recorder"), (1, "notes"), (2, "settings")):
            api.call("wait", {"frames": 3})
            path = "/tmp/ai_recorder_sim_%s.png" % name
            failures += check(api, "the picture of the %s page" % name, "screenshot",
                              {"path": path, "format": "png"})

        api.close()
    finally:
        if not args.keep:
            for process in (app, sim):
                if process is not None:
                    process.terminate()

    print()
    if failures:
        print("%d simulator checks failed" % failures)
        return 1
    print("every simulator check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
