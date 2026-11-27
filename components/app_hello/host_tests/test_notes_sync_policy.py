import re
import unittest
from pathlib import Path


NOTES_SYNC_SOURCE = Path(__file__).parent.parent / "meeting_notes_sync.c"
HELLO_ROOT = Path(__file__).parent.parent


def _source() -> str:
    return NOTES_SYNC_SOURCE.read_text(encoding="utf-8")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class NotesSyncStackPolicyTest(unittest.TestCase):
    """The notes sync task must survive the TLS and DNS call chain.

    The task used to run with an 8 KB stack. The esp_http_client_open()
    chain (TLS handshake plus lwIP DNS resolution) needs more than that, so
    the stack overflowed inside netconn_gethostbyname and the board rebooted
    in a loop. The stack size now comes from a named constant so the reason
    for the size is recorded next to it.
    """

    def test_stack_size_comes_from_a_named_constant(self):
        source = _source()
        self.assertIn("#define NOTES_SYNC_STACK", source)
        self.assertNotRegex(source, r"xTaskCreate\([^;]*\"notes_sync\"[^;]*,\s*\d{3,}\s*,")

    def test_stack_size_is_at_least_16384(self):
        source = _source()
        match = re.search(r"#define\s+NOTES_SYNC_STACK\s+(\d+)", source)
        self.assertIsNotNone(match, "NOTES_SYNC_STACK must be a numeric constant")
        self.assertGreaterEqual(int(match.group(1)), 16384)

    def test_task_is_created_with_the_constant(self):
        source = _source()
        create = _body(source, "xTaskCreate(sync_task", ")")
        self.assertIn("NOTES_SYNC_STACK", create)

    def test_sync_task_ends_with_task_delete(self):
        source = _source()
        task = _body(source, "static void sync_task(void *arg)", "\nvoid meeting_notes_sync_start")
        self.assertIn("vTaskDelete(NULL)", task)
        # The task must leave through vTaskDelete as its last statement.
        statements = [line.strip() for line in task.splitlines() if line.strip()]
        self.assertEqual(statements[-1], "}")
        self.assertEqual(statements[-2], "vTaskDelete(NULL);")

    def test_sync_task_cleans_up_the_client_on_every_path(self):
        """A failed DNS or TLS run must not leave a half-open client behind."""
        source = _source()
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        # Every return after the client exists has to clean it up first.
        self.assertIn("esp_http_client_cleanup(client)", download)
        # Four exits: the open failed, the server has no notes yet, the
        # status was not 200, and the successful read.
        self.assertEqual(download.count("esp_http_client_cleanup(client)"), 4)
        # The successful path closes before it cleans up.
        self.assertLess(
            download.index("esp_http_client_close(client)"),
            download.index("esp_http_client_cleanup(client)", download.index("esp_http_client_close(client)")),
        )

    def test_sync_task_cannot_be_started_twice(self):
        """upload_tick runs every 200 ms, so the start has to be idempotent.

        The sync looks for the minutes again, because the server makes them
        after the recording arrives. The guard is the interval: at most one
        pull per minute, and only one pull in flight.
        """
        source = _source()
        start = _body(source, "void meeting_notes_sync_start(void)", "\nchar *meeting_notes_sync_take")
        self.assertIn("if (s_sync_task)", start)
        self.assertIn("NOTES_SYNC_INTERVAL_MS", start)
        # A failed creation must not leave the module marked as started.
        self.assertIn("s_sync_task = NULL;", start)

    def test_sync_task_releases_the_task_handle_before_it_exits(self):
        """The task drops its handle so no stale pointer is kept."""
        source = _source()
        task = _body(source, "static void sync_task(void *arg)", "\nvoid meeting_notes_sync_start")
        self.assertIn("s_sync_task = NULL;", task)
        self.assertLess(task.index("s_sync_task = NULL;"), task.index("vTaskDelete(NULL)"))

    def test_the_sync_does_not_run_in_a_loop(self):
        """The sync must not download in a loop.

        The minutes are made on the server after the recording arrives, so
        the device has to look again after it uploaded one. It must not do
        that on every tick: the pulls are a minute apart, and the notes are
        merged into the local document by their id, so the same card is
        updated instead of added twice.
        """
        source = _source()
        start = _body(source, "void meeting_notes_sync_start(void)", "\nchar *meeting_notes_sync_take")
        self.assertIn("NOTES_SYNC_INTERVAL_MS", start)
        self.assertIn("s_last_start_us", start)


class NotesArrivalPolicyTest(unittest.TestCase):
    """The minutes arrive at any time, not only while the page is open.

    The document the server sends every minute used to be taken by the
    minutes page. Until the page was opened it sat in the queue, and the
    next document was thrown away with "the notes queue was full". The
    minutes are taken by the tick instead, and the copy on the flash is
    written only when they really changed.
    """

    def _app(self) -> str:
        return _read(HELLO_ROOT / "app_hello.c")

    def test_the_minutes_are_taken_outside_the_page(self):
        source = self._app()
        tick = _body(source, "static void upload_tick(", "\nstatic void timer_tick")
        self.assertIn("take_notes(state);", tick)
        # The page paints the minutes, it does not take them.
        self.assertLess(tick.index("take_notes(state);"), tick.index("if (state->scene_id == 1)"))

    def test_the_take_merges_and_frees_the_document(self):
        source = self._app()
        take = _body(source, "static void take_notes(struct app_hello *state)\n{", "\nstatic void paint_notes")
        self.assertIn("meeting_notes_sync_take()", take)
        self.assertIn("meeting_notes_merge_document(", take)
        self.assertIn("free(delivered);", take)

    def test_the_flash_is_only_written_when_the_minutes_changed(self):
        source = self._app()
        take = _body(source, "static void take_notes(struct app_hello *state)\n{", "\nstatic void paint_notes")
        self.assertIn("meeting_notes_fingerprint(", take)
        self.assertIn("meeting_notes_save(", take)
        # The write has to be behind the comparison, not next to it.
        self.assertLess(take.index("meeting_notes_fingerprint("), take.index("meeting_notes_save("))


class NotesSyncLoggingPolicyTest(unittest.TestCase):
    """The notes sync must not fail silently.

    The sync_task had no log lines at all. A device with an IP address and
    an inbox that never saw it looked exactly like a device that was simply
    idle, so there was no way to tell a dead link from a missing clock.
    """

    def test_the_module_has_a_log_tag(self):
        source = _source()
        self.assertIn("static const char *TAG = ", source)

    def test_the_sync_task_logs_its_start(self):
        source = _source()
        task = _body(source, "static void sync_task(void *arg)", "\nvoid meeting_notes_sync_start")
        self.assertIn("sync task started", task)

    def test_the_sync_task_logs_its_end(self):
        source = _source()
        task = _body(source, "static void sync_task(void *arg)", "\nvoid meeting_notes_sync_start")
        self.assertIn("sync ended", task)

    def test_the_status_code_is_logged(self):
        source = _source()
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("status=", download)

    def test_the_client_error_is_logged(self):
        source = _source()
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("esp_err_to_name", download)

    def test_an_empty_document_is_not_a_failure(self):
        """An inbox with no card yet answers with an empty document.

        The minutes are made on the server after the recording arrives, so an
        empty answer is the normal answer until then. It means the same as
        a 404, and the device looks again. Reporting it as a failure put an
        error on the screen every minute.
        """
        source = _source()
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("if (total <= 0)", download)
        empty = _body(download, "if (total <= 0)", "\n}")
        self.assertIn("the server has no notes yet", empty)
        self.assertIn("return 1;", empty)
        self.assertNotIn("ESP_LOGE", empty)

    def test_a_missing_clock_is_reported(self):
        """The certificate bundle checks the validity window of the server
        certificate against the system clock, so a device without a wall
        clock cannot do the handshake."""
        source = _source()
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("wait_for_clock()", download)
        self.assertIn("app_clock_valid()", source)
        # The wait has to be bounded, and it has to say what went wrong.
        self.assertIn("NOTES_SYNC_CLOCK_WAIT_MS", source)
        self.assertIn("the clock is not set", source)


if __name__ == "__main__":
    unittest.main()
