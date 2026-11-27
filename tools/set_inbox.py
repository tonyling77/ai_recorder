#!/usr/bin/env python3
"""Point the board at the inbox on this machine.

The board finds the inbox on its own: the inbox announces itself on the
network and the board asks before it uses one. This script is for the
networks where that does not work — a guest Wi-Fi, a network with client
isolation, a company network — where the address has to be given over the
link that carried the update.

    python3 tools/set_inbox.py --project .

The board keeps the address, so it survives a restart and a firmware
update. The name of this machine is used rather than its address, so the
address of the machine may change under the board.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
from pathlib import Path

# The service the recorder application registers for this. The protocol
# reserves 0x1001 for the pointer profile and 0x7fff for recovery.
SERVICE_ID = "0x1002"
METHOD_ID = "1"


def lan_origin(port: int) -> str:
    """The address of this machine, as a name the board can resolve.

    A name that ends in .local is resolved by the board with a multicast
    mDNS query, so it follows the machine. A machine whose name does not
    end that way has no name the board can resolve on its own, so its
    address is used."""
    name = socket.gethostname().strip()
    if name.endswith(".local"):
        return f"http://{name}:{port}"
    try:
        address = socket.gethostbyname(name)
    except OSError:
        address = "127.0.0.1"
    return f"http://{address}:{port}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", default=".",
                        help="the project the board belongs to")
    parser.add_argument("--origin", default="",
                        help="the base URL of the inbox; the name of this machine by default")
    parser.add_argument("--port", type=int, default=8765, help="the port of the inbox")
    parser.add_argument("--device-id", default="", help="the device; the only one when there is one")
    arguments = parser.parse_args()

    origin = arguments.origin or lan_origin(arguments.port)
    workspace = Path(__file__).resolve().parents[1]
    command = [
        sys.executable, str(workspace / "mosaico.py"), "iris", "rpc",
        SERVICE_ID, METHOD_ID,
        "--payload", origin,
        "--project", arguments.project,
    ]
    if arguments.device_id:
        command.extend(["--device-id", arguments.device_id])
    print(f"the board is told that the inbox is at {origin}", flush=True)
    return subprocess.call(command, cwd=workspace)


if __name__ == "__main__":
    raise SystemExit(main())
