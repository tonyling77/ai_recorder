import hashlib
import subprocess
import sys
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from tempfile import TemporaryDirectory

from inbox import Inbox, _default_config, lan_origin, serve


class InboxTest(unittest.TestCase):
    def test_summary_waits_for_a_contiguous_last_slice(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root))
            server = serve(inbox, "127.0.0.1", 0)
            port = server.server_address[1]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                self._post(port, "/health", b"", {"X-Device-Id": "board-1"})
                self.assertIn("board-1", (Path(root) / "health.log").read_text(encoding="utf-8"))

                body = b"OggS-slice"
                digest = hashlib.sha256(body).hexdigest()
                bad = self._post(
                    port,
                    "/v1/slices",
                    body,
                    self._headers("9", 0, digest + "00", False),
                    expect=400,
                )
                self.assertEqual(bad, b"rejected")

                stored = self._post(port, "/v1/slices", body, self._headers("9", 0, digest, False))
                self.assertEqual(stored, b"stored")
                self.assertFalse(inbox.ready_for_summary("9", "board-1"))

                missing = self._post(port, "/v1/slices", body, self._headers("9", 2, digest, True))
                self.assertEqual(missing, b"stored")
                self.assertFalse(inbox.ready_for_summary("9", "board-1"))

                ready = self._post(port, "/v1/slices", body, self._headers("9", 1, digest, False))
                self.assertEqual(ready, b"ready")
                self.assertTrue(inbox.ready_for_summary("9", "board-1"))
            finally:
                server.shutdown()
                server.server_close()

    def test_summary_is_written_only_after_the_last_slice(self):
        with TemporaryDirectory() as root:
            calls = []

            def transcribe(audio, name):
                calls.append(name)
                return {"0.ogg": "明天交报告", "1.ogg": "讨论了进度", "2.ogg": "下周一评审"}[name]

            def summarize(text):
                calls.append(text)
                return "# 纪要\n\n## 待办\n\n- 明天交报告\n"

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=summarize)
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("4", 0, 20, digest, False, body)
            self.assertEqual(calls, [])
            self.assertFalse((Path(root) / "4" / "summary.md").exists())

            inbox.accept_slice("4", 2, 20, digest, True, body)
            self.assertEqual(calls, [])

            inbox.accept_slice("4", 1, 20, digest, False, body)
            self.assertEqual(calls, ["0.ogg", "1.ogg", "2.ogg", "明天交报告\n讨论了进度\n下周一评审"])
            summary = (Path(root) / "4" / "summary.md").read_text(encoding="utf-8")
            self.assertIn("明天交报告", summary)
            transcript = (Path(root) / "4" / "transcript.txt").read_text(encoding="utf-8")
            self.assertIn("明天交报告", transcript)

    def test_a_new_checksum_replaces_the_summary(self):
        with TemporaryDirectory() as root:
            seen = []

            def transcribe(audio, name):
                seen.append(audio)
                return "第一遍" if audio == b"old-audio" else "第二遍"

            def summarize(text):
                return f"# 纪要\n\n{text}\n"

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=summarize)
            first = b"old-audio"
            inbox.accept_slice("1", 0, 20, hashlib.sha256(first).hexdigest(), True, first)
            repeat = hashlib.sha256(first).hexdigest()
            inbox.accept_slice("1", 0, 20, repeat, True, first)
            self.assertEqual(seen, [first])

            second = b"new-audio"
            inbox.accept_slice("1", 0, 20, hashlib.sha256(second).hexdigest(), True, second)
            transcript = (Path(root) / "1" / "transcript.txt").read_text(encoding="utf-8")
            self.assertEqual(transcript, "第二遍")
            self.assertIn("第二遍", (Path(root) / "1" / "summary.md").read_text(encoding="utf-8"))
            self.assertEqual(seen, [first, second])

    def test_device_id_namespaces_sessions(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root))
            server = serve(inbox, "127.0.0.1", 0)
            port = server.server_address[1]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()

            try:
                headers = self._headers("1", 0, digest, True)
                headers["X-Device-Id"] = "board-a"
                self._post(port, "/v1/slices", body, headers)
                headers["X-Device-Id"] = "board-b"
                self._post(port, "/v1/slices", body, headers)
            finally:
                server.shutdown()
                server.server_close()

            self.assertTrue((Path(root) / "board-a" / "1" / "0.ogg").exists())
            self.assertTrue((Path(root) / "board-b" / "1" / "0.ogg").exists())

    def test_rainmaker_publish_failure_retries_without_losing_local_summary(self):
        with TemporaryDirectory() as root:
            attempts = []

            def publish(device_id, session_id, note):
                attempts.append((device_id, session_id, note))
                if len(attempts) == 1:
                    raise RuntimeError("cloud unavailable")

            inbox = Inbox(
                Path(root),
                transcribe=lambda audio, name: "明确内容",
                summarize=lambda text: "# 测试纪要\n\n无明确待办。\n",
                publish=publish,
            )
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("5", 0, 20, digest, True, body, device_id="board-1")
            folder = Path(root) / "board-1" / "5"
            self.assertTrue((folder / "summary.md").exists())
            self.assertFalse((folder / "rainmaker.synced").exists())

            inbox.accept_slice("5", 0, 20, digest, True, body, device_id="board-1")
            self.assertEqual(len(attempts), 2)
            self.assertEqual(attempts[1][:2], ("board-1", "5"))
            self.assertTrue((folder / "rainmaker.synced").exists())

    def test_transcription_failure_still_accepts_the_slice(self):
        with TemporaryDirectory() as root:
            def transcribe(audio, name):
                del audio, name
                raise RuntimeError("transcription rejected (401)")

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=lambda text: "unused")
            server = serve(inbox, "127.0.0.1", 0)
            port = server.server_address[1]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                body = b"OggS-slice"
                digest = hashlib.sha256(body).hexdigest()
                stored = self._post(port, "/v1/slices", body, self._headers("8", 0, digest, True))
                self.assertEqual(stored, b"ready")
                folder = Path(root) / "board-1" / "8"
                self.assertEqual((folder / "0.ogg").read_bytes(), body)
                self.assertFalse((folder / "summary.md").exists())
                self.assertIn("401", (folder / "transcript-error.txt").read_text(encoding="utf-8"))
            finally:
                server.shutdown()
                server.server_close()

    def test_an_unavailable_agent_keeps_the_audio_and_skips_the_summary(self):
        with TemporaryDirectory() as root:
            calls = []

            def transcribe(audio, name):
                calls.append(name)
                return "明天交报告"

            inbox = Inbox(
                Path(root),
                transcribe=transcribe,
                summarize=lambda text: "unused",
                routes={("hermes", "minimax"): lambda text: "# 纪要\n"},
            )
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice(
                "4", 0, 20, digest, True, body, agent="muse", model="minimax"
            )
            folder = Path(root) / "4"
            self.assertEqual((folder / "0.ogg").read_bytes(), body)
            self.assertEqual(calls, [])
            self.assertFalse((folder / "summary.md").exists())
            error = (folder / "route-error.txt").read_text(encoding="utf-8")
            self.assertEqual(error, "muse\n")
            self.assertNotIn("明天交报告", error)

    def test_a_device_that_says_nothing_gets_the_route_of_the_host(self):
        with TemporaryDirectory() as root:
            used = []

            def transcribe(audio, name):
                del audio, name
                return "明天交报告"

            def backend(text):
                used.append(text)
                return "# 纪要\n\n## 待办事项\n\n- 明天交报告\n"

            inbox = Inbox(
                Path(root),
                transcribe=transcribe,
                routes={("direct", "qwen"): backend},
                default_route=("direct", "qwen"),
            )
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("6", 0, 20, digest, True, body)
            folder = Path(root) / "6"
            self.assertEqual(used, ["明天交报告"])
            note = (folder / "summary.md").read_text(encoding="utf-8")
            self.assertTrue(note.startswith("Agent: direct\nModel: qwen\n"))
            self.assertIn("明天交报告", note)

    def test_a_device_that_names_a_route_wins_over_the_host(self):
        with TemporaryDirectory() as root:
            used = []

            def transcribe(audio, name):
                del audio, name
                return "明天交报告"

            def backend(text):
                used.append(text)
                return "# 纪要\n"

            inbox = Inbox(
                Path(root),
                transcribe=transcribe,
                routes={("direct", "qwen"): backend, ("hermes", "minimax"): backend},
                default_route=("direct", "qwen"),
            )
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("7", 0, 20, digest, True, body, agent="hermes", model="minimax")
            note = (Path(root) / "7" / "summary.md").read_text(encoding="utf-8")
            self.assertTrue(note.startswith("Agent: hermes\nModel: minimax\n"))
            self.assertEqual(len(used), 1)

    def test_a_failed_summary_keeps_the_transcript(self):
        with TemporaryDirectory() as root:
            def transcribe(audio, name):
                del audio, name
                return "明天交报告"

            def backend(text):
                del text
                raise RuntimeError("the agent answered nothing")

            inbox = Inbox(
                Path(root),
                transcribe=transcribe,
                routes={("direct", "qwen"): backend},
                default_route=("direct", "qwen"),
            )
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("9", 0, 20, digest, True, body)
            folder = Path(root) / "9"
            self.assertEqual((folder / "transcript.txt").read_text(encoding="utf-8"), "明天交报告")
            self.assertFalse((folder / "summary.md").exists())
            self.assertIn("answered nothing", (folder / "transcript-error.txt").read_text(encoding="utf-8"))

    def test_recorded_at_names_the_audio_file(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root))
            body = b"OggS-slice"
            digest = hashlib.sha256(body).hexdigest()
            inbox.accept_slice("8", 0, 20, digest, True, body, recorded_at="20261111-1126")
            folder = Path(root) / "8"
            self.assertEqual((folder / "20261111-1126-0.ogg").read_bytes(), body)
            self.assertFalse((folder / "0.ogg").exists())

    def _headers(self, session_id, index, digest, is_last):
        return {
            "X-Session-Id": session_id,
            "X-Index": str(index),
            "X-Duration-Ms": "20",
            "X-Sha256": digest,
            "X-Is-Last": "1" if is_last else "0",
            "X-Device-Id": "board-1",
            "Content-Type": "application/ogg",
        }

    def _post(self, port, path, body, headers, expect=200):
        request = urllib.request.Request(
            f"http://127.0.0.1:{port}{path}", data=body, headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(request) as response:
                self.assertEqual(response.status, expect)
                return response.read()
        except urllib.error.HTTPError as error:
            self.assertEqual(error.code, expect)
            return error.read()


class StartupTest(unittest.TestCase):
    """What the inbox says when it cannot start.

    The messages here are the only guide the person who set it up has, so
    they are said in words rather than in a traceback.
    """

    def _run(self, config):
        return subprocess.run(
            [sys.executable, str(Path(__file__).resolve().parent / "inbox.py"),
             "--hermes-config", config, "--port", "0"],
            capture_output=True, text=True, timeout=60,
        )

    def test_a_missing_configuration_is_named(self):
        with TemporaryDirectory() as root:
            missing = Path(root) / "not-there.yaml"
            result = self._run(str(missing))
            self.assertEqual(result.returncode, 2)
            self.assertIn("was not found", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_a_configuration_without_a_model_is_named(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text("route:\n  agent: direct\n", encoding="utf-8")
            result = self._run(str(path))
            self.assertEqual(result.returncode, 2)
            self.assertIn("base_url", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_the_configuration_next_to_the_program_is_found(self):
        with TemporaryDirectory() as root:
            beside = Path(root)
            (beside / "config.yaml").write_text("model: {}\n", encoding="utf-8")
            self.assertEqual(_default_config(beside), beside / "config.yaml")
            (beside / "inbox-config.yaml").write_text("model: {}\n", encoding="utf-8")
            self.assertEqual(_default_config(beside), beside / "inbox-config.yaml")

    def test_the_configuration_of_another_program_is_not_read_behind_the_users_back(self):
        """The inbox reads its own configuration and nothing else.

        The inbox used to fall back to the configuration of another program
        on the machine. That was a convenience for the machine it was
        written on, but a configuration belongs to the program that reads
        it: an installation that has one program must not have it read
        because another program happens to be installed as well. A user who
        wants that configuration says so with --config.
        """
        with TemporaryDirectory() as root:
            beside = Path(root)
            self.assertEqual(_default_config(beside), beside / "inbox-config.yaml")


class LanOriginTest(unittest.TestCase):
    """The origin the developer puts in .env.

    The address of a laptop changes when the lease runs out, and the board
    would lose the inbox. A name that ends in .local is resolved by the
    board with a multicast mDNS query, so it follows the machine.
    """

    def test_a_local_name_is_used_as_it_is(self):
        origin = lan_origin(8765)
        self.assertTrue(origin.startswith("http://"))
        self.assertTrue(origin.endswith(":8765"))
        host = origin[len("http://"):-len(":8765")]
        self.assertTrue(host.endswith(".local") or "." in host,
                        "the origin names a host the board can resolve")

    def test_the_port_is_the_one_that_is_listening(self):
        self.assertTrue(lan_origin(1234).endswith(":1234"))


if __name__ == "__main__":
    unittest.main()
