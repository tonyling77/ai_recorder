"""The retry of the recordings that failed to transcribe.

The device marks a slice as uploaded once the inbox stored it, so a
recording that failed here is never sent again. These tests are about the
inbox trying it by itself.
"""

import hashlib
import io
import threading
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory

from inbox import Inbox, RetryWorker, failure_count


class RetryTest(unittest.TestCase):
    """A recording that failed is tried again by the inbox itself."""

    def test_a_failed_recording_is_tried_again(self):
        with TemporaryDirectory() as root:
            attempts = []

            def transcribe(audio, name):
                attempts.append(name)
                if len(attempts) == 1:
                    raise RuntimeError("transcription rejected (401)")
                return "明天交报告"

            inbox = Inbox(Path(root), transcribe=transcribe,
                          summarize=lambda text: "# 纪要\n\n- 明天交报告\n")
            inbox.accept_slice("7", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "7"
            self.assertTrue((folder / "transcript-error.txt").exists())
            self.assertFalse((folder / "summary.md").exists())

            RetryWorker(inbox, base=0).run_once()

            self.assertTrue((folder / "summary.md").exists())
            self.assertTrue((folder / "device-note.txt").exists())
            self.assertFalse((folder / "transcript-error.txt").exists())
            self.assertEqual(len(attempts), 2)

    def test_the_failure_file_says_how_many_times_it_failed(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "3"

            lines = (folder / "transcript-error.txt").read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "1")
            self.assertIn("401", lines[1])

            RetryWorker(inbox, base=60).retry_session("board-1", "3", folder)

            lines = (folder / "transcript-error.txt").read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "2")
            self.assertIn("401", lines[1])

    def test_the_wait_grows_with_the_number_of_failures(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "3"
            worker = RetryWorker(inbox, base=60, cap=1800)

            first = worker.delay(folder)
            worker.retry_session("board-1", "3", folder)
            second = worker.delay(folder)

            self.assertEqual(first, 60)
            self.assertEqual(second, 120)
            self.assertGreater(second, first)

    def test_the_wait_stops_at_the_cap(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "3"
            worker = RetryWorker(inbox, base=60, cap=1800)

            for _ in range(40):
                worker.retry_session("board-1", "3", folder)

            self.assertEqual(worker.delay(folder), 1800)
            self.assertEqual(failure_count(folder), 41)

    def test_a_recording_that_just_failed_waits_before_it_is_tried(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")

            self.assertEqual(RetryWorker(inbox, base=60).pending(), [])

    def test_the_reason_of_the_failure_is_said(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "3"
            worker = RetryWorker(inbox, base=60)

            said = self._said(lambda: worker.retry_session("board-1", "3", folder))

            self.assertIn("board-1/3", said)
            self.assertIn("401", said)

    def test_a_route_that_cannot_be_taken_is_counted(self):
        with TemporaryDirectory() as root:
            def transcribe(audio, name):
                return "明天交报告"

            inbox = Inbox(
                Path(root),
                transcribe=transcribe,
                routes={("hermes", "minimax"): lambda text: "# 纪要\n"},
            )
            body = b"OggS-slice"
            inbox.accept_slice("4", 0, 20, self._digest(body), True, body,
                               device_id="board-1", agent="muse", model="minimax")
            folder = Path(root) / "board-1" / "4"
            self.assertTrue((folder / "route-error.txt").exists())
            self.assertFalse((folder / "transcript-error.txt").exists())

            # A recording that failed to transcribe and then lost its
            # route still has to be counted, or it would be tried on
            # every sweep.
            (folder / "transcript-error.txt").write_text("1\ntranscription rejected (401)\n",
                                                           encoding="utf-8")
            worker = RetryWorker(inbox, base=60)
            worker.retry_session("board-1", "4", folder)

            self.assertEqual(failure_count(folder), 2)

    def test_a_recording_that_already_has_a_summary_is_left_alone(self):
        with TemporaryDirectory() as root:
            folder = Path(root) / "board-1" / "3"
            folder.mkdir(parents=True)
            (folder / "transcript-error.txt").write_text("1\nold\n", encoding="utf-8")
            (folder / "summary.md").write_text("# 纪要\n", encoding="utf-8")
            inbox = Inbox(Path(root), transcribe=lambda audio, name: "unused",
                          summarize=lambda text: "unused")

            self.assertEqual(RetryWorker(inbox, base=0).pending(), [])

    def test_a_corrected_slice_keeps_the_recording_on_the_list(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("3", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            folder = Path(root) / "board-1" / "3"

            # A different slice arrives for the same session. The
            # recording still needs a summary, so it must not be dropped
            # from the list of the ones to try again.
            inbox.accept_slice("3", 0, 20, self._digest(b"other"), True, b"other", device_id="board-1")

            self.assertTrue((folder / "transcript-error.txt").exists())
            self.assertEqual(len(RetryWorker(inbox, base=0).pending()), 1)

    def test_a_session_without_a_device_is_found(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            inbox.accept_slice("4", 0, 20, self._digest(b"good"), True, b"good")
            folder = Path(root) / "4"

            self.assertEqual(RetryWorker(inbox, base=0).pending(), [("", "4", folder)])

    def test_one_recording_that_fails_does_not_stop_the_others(self):
        with TemporaryDirectory() as root:
            def transcribe(audio, name):
                if audio == b"bad":
                    raise RuntimeError("transcription rejected (401)")
                return "明天交报告"

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=lambda text: "# 纪要\n")
            inbox.accept_slice("bad", 0, 20, self._digest(b"bad"), True, b"bad", device_id="board-1")
            inbox.accept_slice("good", 0, 20, self._digest(b"good"), True, b"good", device_id="board-1")

            class OneBad(RetryWorker):
                def retry_session(self, device_id, session_id, folder):
                    if session_id == "bad":
                        raise RuntimeError("the folder is gone")
                    return super().retry_session(device_id, session_id, folder)

            OneBad(inbox, base=0).run_once()

            self.assertTrue((Path(root) / "board-1" / "good" / "summary.md").exists())
            self.assertFalse((Path(root) / "board-1" / "bad" / "summary.md").exists())

    def test_the_thread_survives_an_unexpected_error(self):
        with TemporaryDirectory() as root:
            calls = []
            attempts = []

            def transcribe(audio, name):
                attempts.append(name)
                if len(attempts) == 1:
                    raise RuntimeError("transcription rejected (401)")
                return "明天交报告"

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=lambda text: "# 纪要\n")
            inbox.accept_slice("5", 0, 20, self._digest(b"good"), True, b"good", device_id="board-1")
            folder = Path(root) / "board-1" / "5"

            class Broken(RetryWorker):
                def run_once(self):
                    calls.append(1)
                    if len(calls) == 1:
                        raise RuntimeError("the sweep broke")
                    super().run_once()

            worker = Broken(inbox, base=0, period=0.01)
            thread = threading.Thread(target=worker.loop, daemon=True)
            thread.start()
            try:
                self.assertTrue(self._wait_for(lambda: (folder / "summary.md").exists()))
            finally:
                worker.stop.set()
            self.assertGreaterEqual(len(calls), 2)
            self.assertTrue(thread.is_alive())

    def test_the_first_sweep_does_not_wait_for_the_period(self):
        with TemporaryDirectory() as root:
            attempts = []

            def transcribe(audio, name):
                attempts.append(name)
                if len(attempts) == 1:
                    raise RuntimeError("transcription rejected (401)")
                return "明天交报告"

            inbox = Inbox(Path(root), transcribe=transcribe, summarize=lambda text: "# 纪要\n")
            inbox.accept_slice("5", 0, 20, self._digest(b"good"), True, b"good", device_id="board-1")
            folder = Path(root) / "board-1" / "5"

            worker = RetryWorker(inbox, base=0, period=3600)
            thread = threading.Thread(target=worker.loop, daemon=True)
            thread.start()
            try:
                self.assertTrue(self._wait_for(lambda: (folder / "summary.md").exists()))
            finally:
                worker.stop.set()

    def test_the_thread_stops_when_it_is_told_to(self):
        with TemporaryDirectory() as root:
            inbox = Inbox(Path(root), transcribe=self._refuse, summarize=lambda text: "unused")
            worker = RetryWorker(inbox, base=0, period=0.01)
            thread = threading.Thread(target=worker.loop, daemon=True)
            thread.start()
            worker.stop.set()
            thread.join(timeout=5)

            self.assertFalse(thread.is_alive())

    def _refuse(self, audio, name):
        raise RuntimeError("transcription rejected (401)")

    def _digest(self, body: bytes) -> str:
        return hashlib.sha256(body).hexdigest()

    def _wait_for(self, condition, timeout: float = 5.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if condition():
                return True
            time.sleep(0.01)
        return False

    def _said(self, action) -> str:
        """What the inbox said while the action ran.

        The inbox prints what it does, the way it has always said it.
        """
        with redirect_stdout(io.StringIO()) as said:
            action()
        return said.getvalue()


if __name__ == "__main__":
    unittest.main()
