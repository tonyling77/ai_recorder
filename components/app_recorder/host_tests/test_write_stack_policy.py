import re
import unittest
from pathlib import Path


RECORDER_ROOT = Path(__file__).parent.parent
APP_ROOT = RECORDER_ROOT.parent
APP_HELLO_SOURCE = APP_ROOT / "app_hello" / "app_hello.c"
STORE_SOURCE = RECORDER_ROOT / "recorder_store.c"


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class WritePathStackPolicyTest(unittest.TestCase):
    """The task that writes the recording must have room for the write.

    The write path is deeper than it looks. The recording goes into a
    directory, and the commit of that directory compacts it, which erases a
    block of the flash, which makes the wear levelling layer move a page,
    which reads the flash through the SPI bus. Measured with the compiler's
    stack usage report, that chain needs about four kilobytes.

    With a stack of four kilobytes the chain did not fit. The hardware stack
    guard ended the task in the middle of the commit, the board rebooted, and
    the recording was lost because it had never reached the flash. The
    symptom looked like two separate faults, a reboot and a missing file,
    but they were one.

    The stack of the task is therefore twice what the chain needs, so a
    deeper path or a longer recording still fits.

    The upload task has a larger stack, sixteen kilobytes, but for a
    different reason: it runs the TLS handshake and the DNS lookup of the
    HTTPS client. The write task does neither, so the two stacks are sized
    for two different chains and are not compared here.
    """

    def _recorder_stack(self) -> int:
        source = _source(APP_HELLO_SOURCE)
        match = re.search(r"#define\s+APP_HELLO_RECORDER_STACK\s+(\d+)", source)
        self.assertIsNotNone(match, "the recorder task stack must be a number")
        return int(match.group(1))

    def test_the_recorder_task_stack_covers_the_write_path(self):
        """The write path needs about four kilobytes, so the stack must be
        at least eight."""
        self.assertGreaterEqual(self._recorder_stack(), 8192)

    def test_the_recorder_task_stack_is_sized_for_the_write_and_not_for_tls(self):
        """The two tasks are sized for two different chains.

        The write task must not be copied from the upload task, and the
        upload task must not be copied from the write task. This test only
        says that they are not the same number, so that a change to one is
        not mistaken for a change to the other.
        """
        source = _source(APP_HELLO_SOURCE)
        upload = re.search(r"#define\s+APP_HELLO_UPLOAD_STACK\s+(\d+)", source)
        self.assertIsNotNone(upload, "the upload task stack must be a number")
        self.assertNotEqual(
            self._recorder_stack(),
            int(upload.group(1)),
            "the write and the upload task run different chains",
        )


class StorePathBufferPolicyTest(unittest.TestCase):
    """The path buffers belong to the store, not to the stack of the caller.

    The write path put three buffers of five hundred and twelve bytes on
    the stack of the task. That one frame was the largest on the chain, and
    it was the difference between fitting and not fitting.

    The buffers are now in the store instance, which lives on the heap. The
    other functions of the store still keep a path on the stack, so the rule
    is pinned on the function that the write path runs.
    """

    def _put_body(self) -> str:
        source = _source(STORE_SOURCE)
        body = _body(source, "int recorder_store_put(", "\nstatic int stamp_text_ok(")
        self.assertIn("recorder_store_t *store", body)
        return body

    def test_the_write_path_keeps_no_path_buffer_on_the_stack(self):
        """No local array of a path in the function that writes."""
        body = self._put_body()
        locals_ = re.findall(r"^\s+char\s+\w+\[(\d+)\];", body, re.MULTILINE)
        for size in locals_:
            self.assertLessEqual(
                int(size),
                128,
                "a buffer of %s bytes is on the stack of the write path" % size,
            )

    def test_the_write_path_takes_its_paths_from_the_store(self):
        """The three paths come from the instance."""
        body = self._put_body()
        for name in ("dir", "path", "meta_path"):
            self.assertIn(
                "store->%s" % name,
                body,
                "the write path must take %s from the store" % name,
            )

    def test_the_store_carries_the_path_buffers(self):
        """The instance owns them, so the heap holds them instead."""
        source = _source(STORE_SOURCE)
        instance = _body(source, "struct recorder_store {", "\n};")
        for name in ("dir[", "path[", "meta_path["):
            self.assertIn(
                name,
                instance,
                "the store must own %s" % name,
            )


if __name__ == "__main__":
    unittest.main()
