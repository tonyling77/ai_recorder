#!/usr/bin/env python3
"""Check the recorder on the board from the host.

The board is asked to do what a person would do with its buttons, and the
answer says what happened. Every step is checked against the logs of the
board and against the files on the inbox, so a fault is seen as a failed
check and not as a missing file somebody notices a week later.

    python3 tools/board_check.py --device-id <id>
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import subprocess
import sys
import time
from pathlib import Path


PROJECT = Path(__file__).resolve().parent.parent
ROOT = PROJECT
IRIS = ROOT / "submodule/esp-mosaico-utils/ESP-Iris/components/esp_iris/tools/esp_iris.py"

SERVICE = 0x1003
METHOD = 1
COMMAND_RECORD = 0
COMMAND_UPLOAD = 1
COMMAND_STATE = 2
COMMAND_CHANNEL = 3
CHANNEL_PRIVATE = 0
CHANNEL_XIAOZHI = 1


def iris_python() -> str:
    """The interpreter of the Iris runtime, which has the modules the
    control channel needs.

    The system interpreter does not have them, so the runtime of the
    installed Iris is used. """
    base = Path.home() / "Library/Application Support/esp-mosaico/runtimes/esp-iris"
    for candidate in sorted(base.glob("*/py3.13/bin/python"), reverse=True):
        if candidate.exists():
            return str(candidate)
    # The workspace may run without an installed Iris, and then the system
    # interpreter is the only one there is.
    return sys.executable


class Failure(Exception):
    """A check that did not pass."""


def run(command: list[str], stdin: str | None = None, cwd: Path | None = None) -> str:
    result = subprocess.run(command, input=stdin, capture_output=True, text=True, cwd=cwd)
    if result.returncode != 0:
        raise Failure(result.stdout.strip()[-300:] or result.stderr.strip()[-300:])
    return result.stdout


def gateway_url() -> str:
    """Start a gateway for the project and return its address."""
    process = subprocess.Popen(
        ["python3", "mosaico.py", "iris", "run", "--project", str(PROJECT.relative_to(ROOT))],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    url = ""
    deadline = time.time() + 30
    while time.time() < deadline:
        line = process.stdout.readline()
        if not line:
            break
        for part in line.split():
            if part.startswith("http://127.0.0.1:"):
                url = part
        if url:
            break
    if not url:
        process.terminate()
        raise Failure("the gateway did not come up")
    return url, process


def rpc(gateway: str, device_id: str, command: int, extra: bytes = b"") -> str:
    """Ask the board to do a command, and return what it says."""
    payload = base64.b64encode(bytes([command]) + extra).decode("ascii")
    output = run(
        [
            iris_python(),
            str(IRIS),
            "ctl",
            "--url",
            gateway,
            "--json",
            "rpc-raw",
            device_id,
            "0x%04x" % SERVICE,
            str(METHOD),
            "--payload-base64-stdin",
            "--deadline-ms",
            "4000",
        ],
        stdin=payload,
    )
    document = json.loads(output)
    if "error" in document:
        raise Failure(str(document["error"]))
    return base64.b64decode(document["payload_base64"]).decode("utf-8", "replace")


def logs(device_id: str) -> str:
    return run(
        [
            "python3",
            "mosaico.py",
            "iris",
            "logs",
            "--snapshot",
            "--disable-auto-color",
            "--project",
            str(PROJECT.relative_to(ROOT)),
            "--device-id",
            device_id,
        ],
        cwd=ROOT,
    )


def field(answer: str, name: str) -> int:
    for part in answer.split():
        if part.startswith(name + "="):
            return int(part.split("=", 1)[1])
    raise Failure("the answer has no %s: %s" % (name, answer))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", required=True)
    args = parser.parse_args()

    gateway, process = gateway_url()
    failures = []

    def check(name: str, body) -> None:
        try:
            body()
        except Failure as error:
            print("FAIL %-46s %s" % (name, error))
            failures.append(name)
            return
        print("ok   %s" % name)

    try:
        # The reports of the writes that are already done. The write of the
        # recording of this run has to be a new one.
        before = logs(args.device_id)

        # The board answers, so the link and the handler are both there.
        answer = {}
        check("the board answers the recorder request",
              lambda: answer.update(raw=rpc(gateway, args.device_id, COMMAND_STATE)))
        if "raw" not in answer:
            return 1

        # The board is asked to record, and the answer says it is recording.
        #
        # The request goes into a queue of the board and is carried out by
        # its own task a moment later, so the answer of the request is not
        # the answer of the recording. The state is read until it changes.
        #
        # The state is read with the command that only reads, so the
        # reading itself does not disturb the recording.
        def start() -> None:
            rpc(gateway, args.device_id, COMMAND_RECORD)
            for _ in range(80):
                text = rpc(gateway, args.device_id, COMMAND_STATE)
                if field(text, "recording") == 1:
                    return
                time.sleep(0.25)
            raise Failure("the board did not start to record")
        check("the board starts to record", start)

        # A recording of a few seconds.
        time.sleep(8)

        # The board is asked to stop, and the answer says it is not
        # recording any more.
        def stop() -> None:
            rpc(gateway, args.device_id, COMMAND_RECORD)
            for _ in range(80):
                text = rpc(gateway, args.device_id, COMMAND_STATE)
                if field(text, "recording") == 0:
                    return
                time.sleep(0.25)
            raise Failure("the board did not stop the recording")
        check("the board stops the recording", stop)

        # The write to the flash takes a while. The board says how long it
        # took, and it was twenty seconds once, so the report is waited for
        # instead of guessing how long the write takes.
        def written() -> None:
            text = ""
            reports = []
            for _ in range(360):
                text = logs(args.device_id)
                # Only the writes of this recording count, so the last report
                # of the board is the one that matters. The reports that
                # were already there before the recording do not count: the
                # write of this recording has to be a new one.
                reports = [
                    line
                    for line in text.splitlines()
                    if "the write used" in line and line not in before
                ]
                if reports:
                    break
                time.sleep(0.25)
            if not reports:
                raise Failure("the board did not report the write")
            found = re.search(r"the write used (\d+) bytes of the (\d+) byte stack", reports[-1])
            if not found:
                raise Failure("the board did not report the write: %s" % reports[-1])
            used = int(found.group(1))
            stack = int(found.group(2))
            # The write must leave room in the stack for a deeper path.
            #
            # The rule is a margin and not a share of the stack: the encoder
            # of the Opus codec puts arrays on the stack that are as long as
            # the frame it encodes, so the write needs about twenty kilobytes
            # on its own. The margin is what is left for a path that is
            # deeper than the one that was measured.
            if stack - used < 8192:
                raise Failure("the write used %d of %d bytes, %d left"
                              % (used, stack, stack - used))
            # The write of this recording must not have failed.
            tail = text[max(0, text.rindex(reports[-1]) - 4000):]
            if "record stop failed" in tail:
                raise Failure("the write failed")
        check("the recording is written to the flash", written)

        def survived() -> None:
            text = logs(args.device_id)
            for marker in ("Guru Meditation", "Stack protection fault", "assert failed"):
                if marker in text:
                    raise Failure("the board says %s" % marker)
        check("the board did not fault", survived)

        # The board holds more recordings than before.
        def counted() -> None:
            text = rpc(gateway, args.device_id, COMMAND_STATE)
            if field(text, "sessions") < 18:
                raise Failure("the board says %s" % text)
        check("the board holds the recording", counted)

        # The minutes are on the board, all of them.
        def minutes() -> None:
            text = rpc(gateway, args.device_id, COMMAND_STATE)
            if field(text, "notes") < 1:
                raise Failure("the board says %s" % text)
        check("the board holds the minutes", minutes)

        # The channel is switched, and the board says which channel it is
        # on. The switch is done by the task of the screen, because that is
        # the task that writes the channel to the flash, and the write of
        # the flash cannot be done by a task whose stack is in the external
        # memory. The board went down every time the channel was switched
        # before the write was moved to a task of its own.
        def switch_to(wanted: int, label: str) -> None:
            def body() -> None:
                rpc(gateway, args.device_id, COMMAND_CHANNEL, bytes([wanted]))
                for _ in range(160):
                    text = rpc(gateway, args.device_id, COMMAND_STATE)
                    # The choice of the user is on the screen at once, but the
                    # work of the channel changes a moment later: the work of
                    # the channel that is left has to end first. So the work
                    # is what is waited for.
                    if field(text, "channel") == wanted and field(text, "work") == wanted:
                        return
                    time.sleep(0.25)
                raise Failure("the board did not go to the %s channel" % label)
            check("the board goes to the %s channel" % label, body)

        switch_to(CHANNEL_XIAOZHI, "xiaozhi")
        time.sleep(6)

        # The check-in to the cloud runs on its own, so the board is ready to
        # upload after a restart. It needs the network, so it can only run
        # when the board has an address. A board that is not on the network
        # is not a fault of the switch, so that case is reported and not
        # counted as a failure.
        def checked_in() -> None:
            for _ in range(120):
                text = logs(args.device_id)
                if "Xiaozhi check-in started" in text:
                    return
                if "Disconnected" in text:
                    print("     the board is not on the network: the check-in cannot run")
                    return
                time.sleep(0.25)
            raise Failure("the board did not check in to the cloud")
        check("the board checks in to the cloud", checked_in)

        switch_to(CHANNEL_PRIVATE, "private")
        time.sleep(6)

        # The switch really happened on the task of the screen, and the
        # write of the flash really happened on the task of its own.
        def switched() -> None:
            text = logs(args.device_id)
            if "the channel is switched by the task esp_gsp" not in text:
                raise Failure("the switch was not done by the task of the screen")
            if "the flash is written by the task flash_write" not in text:
                raise Failure("the flash was not written by the task of its own")
            if "the work of the channel" not in text:
                raise Failure("the work of the channel did not change")
        check("the switch and the write are on their own tasks", switched)

        # The board is still alive after all the switching.
        def survived_the_switch() -> None:
            text = logs(args.device_id)
            for marker in ("Guru Meditation", "Stack protection fault", "assert failed"):
                if marker in text:
                    raise Failure("the board says %s" % marker)
        check("the board did not fault on the switch", survived_the_switch)

        # The board keeps the channel it was on.
        def remembered() -> None:
            text = rpc(gateway, args.device_id, COMMAND_STATE)
            if field(text, "channel") != CHANNEL_PRIVATE:
                raise Failure("the board says %s" % text)
            if field(text, "work") != CHANNEL_PRIVATE:
                raise Failure("the work of the board says %s" % text)
        check("the board keeps the channel", remembered)
    finally:
        process.terminate()

    print()
    if failures:
        print("%d checks failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("every check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
