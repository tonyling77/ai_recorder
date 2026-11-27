"""LAN inbox for AI Recorder slices.

The device posts one closed Ogg Opus slice at a time. A summary is requested
only after the indexes are contiguous and the last slice is marked.

A recording that failed here is tried again by the inbox itself, because the
device believes a slice is stored once the inbox said so, and never sends it
a second time.
"""

from __future__ import annotations

import atexit
import hashlib
import json
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from agent_route import AgentUnavailable, RouteError, ensure_route, route_summary
from device_note import card_from_summary

# How a recording that failed to transcribe is tried again. The wait grows
# with the number of failures, because an account that is out of credit
# would otherwise burn a request every minute, and it stops at the cap so
# that a recording that keeps failing is still tried.
RETRY_PERIOD_SECONDS = 30
RETRY_BASE_SECONDS = 60
RETRY_MAX_SECONDS = 1800


def failure_count(folder: Path) -> int:
    """How many times the recording in this folder has failed.

    The count is the first line of the error file, so it is in the file
    rather than in the memory of the process and survives a restart. A file
    that has only a reason there was written by an older inbox and counts
    as no failure yet.
    """
    try:
        lines = (folder / "transcript-error.txt").read_text(encoding="utf-8").splitlines()
    except OSError:
        return 0
    if len(lines) < 2:
        return 0
    try:
        return max(int(lines[0].strip()), 0)
    except ValueError:
        return 0


def write_failure(folder: Path, message: str) -> None:
    """Record that the recording failed, and how many times it has failed.

    The count is on the first line so that the inbox knows how long to wait
    before it tries again.
    """
    count = failure_count(folder) + 1
    (folder / "transcript-error.txt").write_text(f"{count}\n{message}\n", encoding="utf-8")


def session_names(root: Path, folder: Path) -> tuple[str, str] | None:
    """The device and the session a folder belongs to.

    The folders are one level deep for a session without a device and two
    levels deep for a session with one, and both are in use.
    """
    parts = folder.parts[len(root.parts):]
    if len(parts) == 1:
        return "", parts[0]
    if len(parts) >= 2:
        return parts[-2], parts[-1]
    return None


class Inbox:
    def __init__(self, root: Path, transcribe=None, summarize=None, publish=None, routes=None,
                 default_route=None):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.health_log = self.root / "health.log"
        self.transcribe = transcribe
        self.summarize = summarize
        self.publish = publish
        self.routes = routes
        # The route the host asks for. The device names its own when it
        # sends the headers, and this is the one in use otherwise.
        self.default_route = default_route or ("direct", "")

    def health(self, device_id: str) -> None:
        if not device_id:
            raise ValueError("device id required")
        with self.health_log.open("a", encoding="utf-8") as handle:
            handle.write(device_id + "\n")

    def _session_folder(self, session_id: str, device_id: str = "") -> Path:
        for value in (device_id, session_id):
            if value and not all(char.isalnum() or char in "-_." for char in value):
                raise ValueError("invalid identifier")
        return self.root / device_id / session_id if device_id else self.root / session_id

    def accept_slice(
        self,
        session_id: str,
        index: int,
        duration_ms: int,
        sha256: str,
        is_last: bool,
        data: bytes,
        device_id: str = "",
        agent: str = "",
        model: str = "",
        recorded_at: str = "",
    ) -> None:
        digest = hashlib.sha256(data).hexdigest()
        if digest != sha256:
            raise ValueError("checksum mismatch")
        folder = self._session_folder(session_id, device_id)
        folder.mkdir(parents=True, exist_ok=True)
        audio_name = f"{recorded_at}-{index}.ogg" if re.fullmatch(r"\d{8}-\d{4}", recorded_at or "") else f"{index}.ogg"
        previous = folder / f"{index}.json"
        changed = True
        if previous.exists():
            try:
                old = json.loads(previous.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                old = {}
            changed = old.get("sha256") != sha256
        (folder / audio_name).write_bytes(data)
        meta = {
            "session_id": session_id,
            "index": index,
            "duration_ms": duration_ms,
            "sha256": sha256,
            "is_last": is_last,
            "agent": agent,
            "model": model,
            "file": audio_name,
            "recorded_at": recorded_at,
        }
        (folder / f"{index}.json").write_text(json.dumps(meta), encoding="utf-8")
        if changed:
            for name in ("summary.md", "transcript.txt", "device-note.txt", "rainmaker.synced", "rainmaker-error.txt"):
                stale = folder / name
                if stale.exists():
                    stale.unlink()
        if self.transcribe and (self.summarize or self.routes) and self.ready_for_summary(session_id, device_id):
            self._write_summary(session_id, device_id)

    def _write_summary(self, session_id: str, device_id: str = "") -> None:
        folder = self._session_folder(session_id, device_id)
        summary_path = folder / "summary.md"
        agent, model = self._selected_route(folder)
        if self.routes is not None:
            try:
                ensure_route(agent, model, self.routes)
            except (RouteError, AgentUnavailable):
                (folder / "route-error.txt").write_text(agent + "\n", encoding="utf-8")
                return
        if not summary_path.exists():
            slices = []
            for path in folder.glob("*.json"):
                slices.append(json.loads(path.read_text(encoding="utf-8")))
            slices.sort(key=lambda item: int(item["index"]))
            parts = []
            try:
                for item in slices:
                    index = int(item["index"])
                    audio_name = str(item.get("file") or f"{index}.ogg")
                    audio = (folder / audio_name).read_bytes()
                    parts.append(self.transcribe(audio, audio_name))
                text = "\n".join(parts)
            except Exception as error:
                message = str(error).strip() or type(error).__name__
                write_failure(folder, message)
                return
            # The transcript is a fact and the summary is made from it, so
            # the transcript is written first. A summary that fails must not
            # take the transcript with it: the transcript is what says the
            # transcription worked.
            (folder / "transcript.txt").write_text(text, encoding="utf-8")
            try:
                if self.routes is not None:
                    note = route_summary(agent, model, text, self.routes)
                else:
                    note = self.summarize(text)
            except Exception as error:
                message = str(error).strip() or type(error).__name__
                write_failure(folder, message)
                return
            summary_path.write_text(note, encoding="utf-8")
            self._write_device_note(folder, device_id, session_id, note)
            error_note = folder / "transcript-error.txt"
            if error_note.exists():
                error_note.unlink()
            route_error = folder / "route-error.txt"
            if route_error.exists():
                route_error.unlink()
        synced = folder / "rainmaker.synced"
        if self.publish and not synced.exists():
            try:
                self.publish(device_id, session_id, summary_path.read_text(encoding="utf-8"))
                synced.write_text("ok\n", encoding="utf-8")
                error_path = folder / "rainmaker-error.txt"
                if error_path.exists():
                    error_path.unlink()
            except Exception as error:
                (folder / "rainmaker-error.txt").write_text(type(error).__name__ + "\n", encoding="utf-8")

    def _write_device_note(self, folder: Path, device_id: str, session_id: str, markdown: str) -> None:
        from datetime import datetime

        note_id = f"{device_id}-{session_id}" if device_id else session_id
        issued = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M")
        (folder / "device-note.txt").write_text(
            card_from_summary(markdown, note_id, issued),
            encoding="utf-8",
        )

    def notes_document(self) -> str:
        cards = []
        for path in sorted(self.root.glob("**/device-note.txt")):
            cards.append(path.read_text(encoding="utf-8").strip())
        return ("\n".join(card for card in cards if card) + "\n") if cards else ""

    def ready_for_summary(self, session_id: str, device_id: str = "") -> bool:
        folder = self._session_folder(session_id, device_id)
        if not folder.is_dir():
            return False
        metas = []
        for path in folder.glob("*.json"):
            metas.append(json.loads(path.read_text(encoding="utf-8")))
        if not metas:
            return False
        by_index = {int(item["index"]): item for item in metas}
        count = len(by_index)
        if set(by_index) != set(range(count)):
            return False
        for index in range(count):
            is_last = bool(by_index[index]["is_last"])
            if is_last != (index + 1 == count):
                return False
        return True

    def _selected_route(self, folder: Path) -> tuple[str, str]:
        """The route that writes this summary.

        The device names it when it sends the headers, and the host names it
        otherwise. A device that says nothing does not get a route of its
        own: the host has one ready."""
        chosen = self.default_route
        for path in folder.glob("*.json"):
            item = json.loads(path.read_text(encoding="utf-8"))
            if item.get("is_last"):
                agent = str(item.get("agent") or "")
                model = str(item.get("model") or "")
                if agent or model:
                    chosen = (agent or chosen[0], model or chosen[1])
        return chosen


class RetryWorker:
    """Tries the recordings that failed to transcribe, again.

    The device marks a slice as uploaded once the inbox stored it, so a
    recording that failed here is never sent again and nobody would know
    that it is lost. The inbox tries it by itself instead.
    """

    def __init__(self, inbox: Inbox, base: int = RETRY_BASE_SECONDS,
                 cap: int = RETRY_MAX_SECONDS, period: int = RETRY_PERIOD_SECONDS):
        self.inbox = inbox
        self.base = base
        self.cap = cap
        self.period = period
        self.stop = threading.Event()

    def delay(self, folder: Path) -> float:
        """How long a failed recording waits before it is tried again.

        The wait grows with the number of failures and stops at the cap.
        A recording that keeps failing is still tried, but not so often
        that it costs more than it is worth.
        """
        count = max(failure_count(folder), 1)
        return min(self.base * count, self.cap)

    def pending(self) -> list[tuple[str, str, Path]]:
        """The failed recordings that are due to be tried again.

        A recording is due when it failed, has no summary yet, and has
        waited longer than the number of its failures asks for. The wait
        is counted from the last failure, which is when the error file
        was written, so it survives a restart of the inbox.
        """
        root = self.inbox.root
        due: list[tuple[str, str, Path]] = []
        for path in sorted(root.glob("**/transcript-error.txt")):
            folder = path.parent
            if (folder / "summary.md").exists():
                continue
            names = session_names(root, folder)
            if names is None:
                continue
            try:
                waited = max(time.time() - path.stat().st_mtime, 0.0)
            except OSError:
                continue
            if waited < self.delay(folder):
                continue
            due.append((names[0], names[1], folder))
        return due

    def retry_session(self, device_id: str, session_id: str, folder: Path) -> bool:
        """Try one failed recording again, through the ordinary path.

        The code that writes the summary of a fresh slice writes it here
        too, so a recording that is recovered this way is not a different
        kind of recording.
        """
        attempt = failure_count(folder)
        label = f"{device_id}/{session_id}" if device_id else session_id
        try:
            self.inbox._write_summary(session_id, device_id)
        except Exception as error:
            message = str(error).strip() or type(error).__name__
            self._record_failure(folder, attempt, message)
            print(f"retry {label} attempt {attempt}: failed, {message}", flush=True)
            return False
        if (folder / "summary.md").exists():
            print(f"retry {label} attempt {attempt}: the summary is written", flush=True)
            return True
        message = self._failure_reason(folder)
        self._record_failure(folder, attempt, message)
        print(f"retry {label} attempt {attempt}: failed, {message}", flush=True)
        return False

    def run_once(self) -> None:
        """Try every recording that is due to be tried again."""
        for device_id, session_id, folder in self.pending():
            try:
                self.retry_session(device_id, session_id, folder)
            except Exception as error:
                # One recording that cannot be written must not keep the
                # others from being tried.
                print(f"retry of {device_id}/{session_id} failed: {error}", flush=True)

    def loop(self) -> None:
        """Try the failed recordings again, for as long as the inbox runs.

        The sweep runs before the first wait, so a recording that failed
        while the inbox was down is tried as soon as the inbox comes back
        and not one period later.
        """
        while not self.stop.is_set():
            try:
                self.run_once()
            except Exception as error:
                # A sweep that fails is no reason to stop: the recording
                # is still there and the next sweep may find it.
                print(f"the retry sweep failed: {error}", flush=True)
            self.stop.wait(self.period)

    def _record_failure(self, folder: Path, before: int, message: str) -> None:
        """Make sure that a failed attempt is counted.

        The count decides how long the next wait is, so an attempt that
        fails without writing an error file of its own has to raise the
        count itself.
        """
        if failure_count(folder) <= before:
            write_failure(folder, message)

    def _failure_reason(self, folder: Path) -> str:
        """Why the recording still has no summary.

        The reason is in the error file, and a route that cannot be
        taken is the usual reason for a file that says nothing else: the
        recording was never transcribed because there was nobody to
        write it.
        """
        try:
            lines = (folder / "transcript-error.txt").read_text(encoding="utf-8").splitlines()
            reason = lines[1].strip() if len(lines) > 1 else ""
        except OSError:
            reason = ""
        if reason:
            return reason
        try:
            agent = (folder / "route-error.txt").read_text(encoding="utf-8").strip()
        except OSError:
            agent = ""
        if agent:
            return "the route is not usable: " + agent
        return "the summary was not written"


class InboxHandler(BaseHTTPRequestHandler):
    inbox: Inbox

    def log_message(self, fmt: str, *args) -> None:
        return

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    def _send(self, status: int, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/v1/notes":
            self._send(200, self.inbox.notes_document().encode("utf-8"))
            return
        self._send(404, b"missing")

    def do_POST(self) -> None:
        try:
            if self.path == "/health":
                self.inbox.health(self.headers.get("X-Device-Id", ""))
                self._send(200, b"ok")
                return
            if self.path == "/v1/slices":
                self.inbox.accept_slice(
                    session_id=self.headers["X-Session-Id"],
                    index=int(self.headers["X-Index"]),
                    duration_ms=int(self.headers["X-Duration-Ms"]),
                    sha256=self.headers["X-Sha256"],
                    is_last=self.headers.get("X-Is-Last", "0") == "1",
                    data=self._read_body(),
                    device_id=self.headers["X-Device-Id"],
                    agent=self.headers.get("X-Agent", ""),
                    model=self.headers.get("X-Model", ""),
                    recorded_at=self.headers.get("X-Recorded-At", ""),
                )
                session_id = self.headers["X-Session-Id"]
                device_id = self.headers["X-Device-Id"]
                ready = b"ready" if self.inbox.ready_for_summary(session_id, device_id) else b"stored"
                self._send(200, ready)
                return
        except (KeyError, ValueError):
            self._send(400, b"rejected")
            return
        self._send(404, b"missing")


def stop_publisher(publisher: subprocess.Popen) -> None:
    """Take the announcement off the network when the inbox stops."""
    if publisher.poll() is not None:
        return
    publisher.terminate()
    try:
        publisher.wait(timeout=3)
    except subprocess.TimeoutExpired:
        publisher.kill()


def serve(inbox: Inbox, host: str = "0.0.0.0", port: int = 8765) -> ThreadingHTTPServer:
    InboxHandler.inbox = inbox
    server = ThreadingHTTPServer((host, port), InboxHandler)
    return server


def publish_service(port: int) -> subprocess.Popen | None:
    """Publish the inbox so that a board on this network can find it.

    The board looks for the service type _esp-inbox._tcp. macOS has dns-sd
    built in and Linux usually has avahi-publish-service, so the inbox needs
    no package for it. The instance name is the name of the machine, and the
    record points at the mDNS name of the machine rather than at its
    address, so the board stores a name that keeps working when the address
    changes.
    """
    name = socket.gethostname().split(".")[0] or "inbox"
    commands = []
    if sys.platform == "darwin" and shutil.which("dns-sd"):
        commands.append(["dns-sd", "-R", name, "_esp-inbox._tcp", "local", str(port)])
    if shutil.which("avahi-publish-service"):
        commands.append(["avahi-publish-service", name, "_esp-inbox._tcp", str(port)])
    for command in commands:
        try:
            return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            continue
    return None


def _default_config(beside: Path | None = None) -> Path:
    """Where the configuration is, when nobody says.

    The configuration belongs to the inbox, so the one next to the program
    comes first. There is no other place to look: an inbox that finds no
    configuration says so, instead of reading the configuration of a
    program that happens to be installed on the machine.
    """
    beside = beside or Path(__file__).resolve().parent
    for name in ("inbox-config.yaml", "config.yaml"):
        candidate = beside / name
        if candidate.is_file():
            return candidate
    return beside / "inbox-config.yaml"


def lan_origin(port: int) -> str:
    """The base URL a board on this network can use to reach this inbox.

    The name of the machine is preferred over its address, because the
    address of a laptop changes when the lease runs out and the board would
    lose the inbox. A name that ends in .local is resolved by the board with
    a multicast mDNS query, so it follows the machine wherever it goes on the
    network.

    A machine whose name does not end in .local has no name the board can
    resolve on its own, so its address is used instead."""
    name = socket.gethostname().strip()
    if name.endswith(".local"):
        return f"http://{name}:{port}"
    try:
        address = socket.gethostbyname(name)
    except OSError:
        address = "127.0.0.1"
    return f"http://{address}:{port}"


if __name__ == "__main__":
    import argparse
    import os
    from datetime import datetime, timezone

    from asr import asr_endpoint, asr_model, transcriber
    from agent_route import RouteError
    from model_api import read_agents, read_model_settings, read_route
    from rainmaker import publish_summary
    from writers import build_backends, ensure_route_usable, resolve_route

    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="inbox-data")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--config", default="",
                        help="the configuration of the inbox; the one next to this program by default")
    parser.add_argument("--hermes-config", default="",
                        help="the same as --config; the name it had when the "
                             "inbox read the configuration of another program")
    parser.add_argument("--agent", default="",
                        help="who writes the summary: the inbox itself with "
                             "--model, or an agent the configuration declares")
    parser.add_argument("--model", default="",
                        help="the model that writes it: minimax, mimo, deepseek, glm, qwen")
    parser.add_argument("--provider", default="",
                        help="the provider of that agent, when the agent has more than one")
    parser.add_argument("--model-id", default="",
                        help="the model id of that agent, when the short name is not enough")
    parser.add_argument("--rainmaker-token-file", default=os.getenv("RAINMAKER_ACCESS_TOKEN_FILE", ""))
    parser.add_argument("--rainmaker-node-id", default=os.getenv("RAINMAKER_NODE_ID", ""))
    parser.add_argument("--retry-period", type=int, default=RETRY_PERIOD_SECONDS,
                        help="how often the failed recordings are tried again, in seconds; 0 turns it off")
    parser.add_argument("--retry-interval", type=int, default=RETRY_BASE_SECONDS,
                        help="how long a failed recording waits before it is tried again, in seconds")
    parser.add_argument("--retry-max-interval", type=int, default=RETRY_MAX_SECONDS,
                        help="the longest wait between two attempts, in seconds")
    args = parser.parse_args()

    chosen = args.config or args.hermes_config
    config_path = Path(chosen) if chosen else _default_config()
    if not config_path.is_file():
        print(f"the configuration was not found: {config_path}", file=sys.stderr, flush=True)
        print("copy config.example.yaml next to the program, fill in the model section, "
              "and point the inbox at it with --config", file=sys.stderr, flush=True)
        raise SystemExit(2)
    try:
        settings = read_model_settings(config_path)
    except ValueError as error:
        print(f"the configuration is not usable: {error}", file=sys.stderr, flush=True)
        print(f"the model section of {config_path} needs base_url, default and api_key",
              file=sys.stderr, flush=True)
        raise SystemExit(2)
    configured = read_route(config_path)
    declared = read_agents(config_path)
    if args.provider:
        configured["provider"] = args.provider
    if args.model_id:
        configured["model_id"] = args.model_id
    try:
        route = resolve_route(settings, args.agent, args.model, configured, declared)
    except RouteError as error:
        print(f"the route of the inbox is not usable: {error}", file=sys.stderr, flush=True)
        raise SystemExit(2)
    try:
        transcribe = transcriber(settings)
    except ValueError as error:
        print(f"the model that transcribes is not usable: {error}", file=sys.stderr, flush=True)
        raise SystemExit(2)
    backends = build_backends(settings, route, configured, declared)
    try:
        ensure_route_usable(route, backends)
    except RouteError as error:
        # The route has to be usable before a recording is lost. A
        # combination that no backend answers is refused here, where the
        # person who set it up can still see it.
        print(f"the route of the inbox is not usable: {error}", file=sys.stderr, flush=True)
        print("the combinations that are usable:", file=sys.stderr, flush=True)
        for agent, model in sorted(backends):
            print(f"  {agent}/{model}", file=sys.stderr, flush=True)
        raise SystemExit(2)

    publish = None
    if args.rainmaker_token_file and args.rainmaker_node_id:
        token_path = Path(args.rainmaker_token_file).expanduser()
        access_token = token_path.read_text(encoding="utf-8").strip()

        def publish(device_id: str, session_id: str, note: str) -> None:
            del device_id
            title = next(
                (line[2:].strip() for line in note.splitlines() if line.startswith("# ") and line[2:].strip()),
                f"Recording {session_id}",
            )
            publish_summary(
                access_token=access_token,
                node_id=args.rainmaker_node_id,
                session_id=int(session_id),
                title=title,
                markdown=note,
                created_at=datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            )

    inbox = Inbox(Path(args.root), transcribe=transcribe, publish=publish,
                  routes=backends, default_route=route)
    server = serve(inbox, args.host, args.port)
    root = Path(args.root).resolve()
    print(f"inbox listening on {args.host}:{args.port}", flush=True)
    print("", flush=True)
    print(f"the recording is transcribed by {asr_model(settings)} at {asr_endpoint(settings)}", flush=True)
    print(f"the summary is written by {route[0]}/{route[1]}", flush=True)
    print(f"the recordings are written to {root}", flush=True)
    print("", flush=True)
    print("Put this in your .env and build the firmware with it:", flush=True)
    print(f"UPLOAD_ORIGIN={lan_origin(args.port)}", flush=True)
    print("", flush=True)
    print("The name ends in .local on purpose: the board resolves such a name", flush=True)
    print("with a multicast mDNS query, so the address of this machine may change", flush=True)
    print("without the board losing the inbox. A fixed address works too.", flush=True)
    print("", flush=True)

    publisher = publish_service(args.port)
    if publisher is None:
        print("The inbox could not announce itself on the network, so a board", flush=True)
        print("will not find it on its own. Point the board at it instead:", flush=True)
        print("  python mosaico.py iris rpc 0x1002 1 --payload \""
              f"{lan_origin(args.port)}\" --project .", flush=True)
    else:
        print("The inbox announces itself as _esp-inbox._tcp, so a board on this", flush=True)
        print("network finds it. It will ask before it uses one.", flush=True)
        atexit.register(stop_publisher, publisher)

    if args.retry_period > 0:
        worker = RetryWorker(inbox, base=args.retry_interval,
                             cap=args.retry_max_interval, period=args.retry_period)
        threading.Thread(target=worker.loop, name="inbox-retry", daemon=True).start()
        print(f"A recording that failed is tried again every {args.retry_period} seconds.", flush=True)
    else:
        print("A recording that failed is not tried again.", flush=True)
    print("", flush=True)

    server.serve_forever()
