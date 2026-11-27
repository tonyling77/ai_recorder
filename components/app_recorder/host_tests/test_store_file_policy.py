import re
import unittest
from pathlib import Path


RECORDER_ROOT = Path(__file__).parent.parent
APP_ROOT = RECORDER_ROOT.parent


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _loop_bodies(text: str):
    """Yield (body, text_after, offset) for every while/for loop.

    The condition may contain nested parentheses, so it is matched by
    scanning for the closing one instead of by a regular expression.
    """
    for match in re.finditer(r"\b(?:while|for)\s*\(", text):
        index = match.end()
        parens = 1
        while index < len(text) and parens > 0:
            if text[index] == "(":
                parens += 1
            elif text[index] == ")":
                parens -= 1
            index += 1
        if parens != 0:
            continue
        while index < len(text) and text[index] in " \t":
            index += 1
        if index >= len(text) or text[index] != "{":
            continue
        body_start = index + 1
        depth = 1
        index = body_start
        while index < len(text) and depth > 0:
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
            index += 1
        if depth != 0:
            continue
        yield text[body_start:index - 1], text[index:], body_start


class StoreFilePolicyTest(unittest.TestCase):
    """A FILE must never be closed twice.

    Closing the same FILE twice corrupts newlib's stdio lock bookkeeping.
    The second close re-enters the lock machinery with a stale nesting count
    and the board aborts with

        assert failed: spinlock_acquire_impl spinlock.h:216 (lock->count == 0)

    The upload path hit this on every press of Upload: the device rebooted
    instead of uploading. The host libc stays silent about a double close,
    so the invariant is pinned on the source instead of on the behaviour.
    """

    SOURCES = (
        RECORDER_ROOT / "recorder_store.c",
        RECORDER_ROOT / "recorder_uplink.c",
        RECORDER_ROOT / "recorder_control.c",
        RECORDER_ROOT / "recorder_mic.c",
        APP_ROOT / "app_hello" / "meeting_notes.c",
        APP_ROOT / "app_hello" / "meeting_notes_sync.c",
        APP_ROOT / "app_hello" / "xiaozhi_upload.c",
    )

    @staticmethod
    def _present():
        """The Xiaozhi sources only exist on the branch that carries them."""
        return tuple(path for path in StoreFilePolicyTest.SOURCES if path.is_file())

    def _violations(self, path: Path):
        text = _source(path)
        found = []
        for body, after, body_start in _loop_bodies(text):
            for close in re.finditer(r"fclose\s*\(\s*(\w+)\s*\)", body):
                variable = close.group(1)
                tail = body[close.end():]
                # A close that leaves the function right away cannot be
                # followed by a second close, so it is safe.
                if re.search(r"\breturn\b", tail):
                    continue
                # Otherwise control can reach the end of the loop, and the
                # code after the loop closes the same pointer again.
                if re.search(r"fclose\s*\(\s*%s\s*\)" % re.escape(variable), after):
                    line = text[:body_start + close.start()].count("\n") + 1
                    found.append((path.name, line, variable))
        return found

    def test_no_file_is_closed_twice(self):
        for path in self._present():
            with self.subTest(source=path.name):
                violations = self._violations(path)
                self.assertEqual(
                    violations, [],
                    "a FILE closed inside a loop must not be closed again after it: %s" % violations,
                )

    def test_the_duration_reader_closes_its_meta_file_once(self):
        """The function that aborted the board on every upload."""
        text = _source(RECORDER_ROOT / "recorder_store.c")
        body = text.split("int recorder_store_duration_ms(", 1)[1].split("\n}\n", 1)[0]
        closes = re.findall(r"fclose\s*\(\s*(\w+)\s*\)", body)
        self.assertEqual(len(closes), 1, "the meta file is closed exactly once")
        self.assertEqual(closes[0], "meta")


class DamagedSlicePolicyTest(unittest.TestCase):
    """A damaged slice must not stop the other sessions from uploading.

    The inventory and the upload read the same meta files. The inventory
    treats a slice it cannot read as the end of that session and carries on,
    so the screen keeps counting the sessions that are intact. The upload
    used to fail outright, so one damaged recording stopped the whole inbox
    from being uploaded and the screen said "Cannot read recording".
    """

    UPLINK = RECORDER_ROOT / "recorder_uplink.c"

    def _load_body(self) -> str:
        text = _source(self.UPLINK)
        return text.split("static int load_closed_slices(", 1)[1].split("\nstatic int recorder_uplink_send_pending", 1)[0]

    def test_a_damaged_slice_does_not_fail_the_whole_upload(self):
        body = self._load_body()
        match = re.search(
            r"recorder_store_duration_ms\([^;]*?\)\s*!=\s*0\)\s*\{(.*?)\n\s*\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "the duration read is checked")
        handler = match.group(1)
        self.assertNotIn("return -1", handler,
                         "a damaged slice must not abort every other session")
        self.assertIn("break", handler, "the damaged session is skipped instead")

    def test_the_damaged_slice_is_reported(self):
        """A silent skip would leave the user with no explanation."""
        body = self._load_body()
        self.assertIn("no readable duration", body)

    def test_the_upload_logs_the_sessions_it_walks(self):
        """The upload had no log line at all, so a failure had no context."""
        text = _source(self.UPLINK)
        self.assertIn("upload: session=", text)

    def test_a_finished_session_is_not_an_error(self):
        """The end of a session is not a failure.

        Once the last slice of a session is uploaded, the index moves past
        the end of that session. Reading the checksum of the next index
        fails, and that used to be reported as "Cannot read recording" even
        though every slice had been uploaded and acknowledged.
        """
        text = _source(self.UPLINK)
        body = text.split("int recorder_uplink_send_pending(", 1)[1]
        match = re.search(
            r"recorder_store_sha256\([^;]*?\)\s*!=\s*0\)\s*\{(.*?)\n\s*\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "the checksum read is checked")
        handler = match.group(1)
        self.assertNotIn("upload_fail", handler,
                         "the end of a session must not fail the upload")
        self.assertIn("break", handler, "the finished session ends the loop")

    def test_the_upload_never_fails_after_the_last_slice(self):
        """The upload has to end on a successful slice.

        This is the shape that broke: the slice was uploaded and
        acknowledged, and the upload still reported a failure afterwards.
        The only checksum read that may still fail is the one in the
        finished-session path, and that one breaks out of the loop.
        """
        text = _source(self.UPLINK)
        body = text.split("int recorder_uplink_send_pending(", 1)[1]
        checks = re.findall(
            r"recorder_store_sha256\([^;]*?\)\s*!=\s*0\)\s*\{(.*?)\n\s*\}", body, re.S)
        self.assertTrue(checks, "the checksum read is checked")
        for handler in checks:
            self.assertNotIn("upload_fail", handler,
                             "the end of a session must not fail the upload")
            self.assertIn("break", handler, "the finished session ends the loop")


if __name__ == "__main__":
    unittest.main()
