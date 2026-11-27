import unittest
from pathlib import Path


RECORDER_ROOT = Path(__file__).parent.parent
PROJECT_ROOT = RECORDER_ROOT.parents[1]
ONBOARD_ROOT = PROJECT_ROOT / "components" / "app_onboard"
HELLO_ROOT = PROJECT_ROOT / "components" / "app_hello"
CLOCK_ROOT = PROJECT_ROOT / "components" / "app_clock"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class UploadClockPolicyTest(unittest.TestCase):
    """The upload must not run before the clock is set.

    The certificate bundle validates the validity window of the server
    certificate against the system clock. Without a wall clock the handshake
    fails with a TLS error that says nothing about the real problem, so the
    upload has to refuse to start and say that the clock is missing.
    """

    def test_the_upload_consults_the_clock(self):
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        self.assertIn("app_clock_valid()", source)

    def test_the_clock_is_checked_before_the_first_request(self):
        """The health check is the first request of the upload, so the clock
        has to be checked before it goes out."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        send = _body(source, "int recorder_uplink_send_pending(", "\\nconst char *recorder_uplink_failure")
        self.assertIn("uplink_clock_ready()", send)
        self.assertLess(send.index("uplink_clock_ready()"), send.index("recorder_upload_prepare("))
        self.assertLess(send.index("uplink_clock_ready()"), send.index("post_request("))

    def test_the_clock_check_guards_the_health_request(self):
        """The helper sits in front of the request helper."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_clock_ready(void)", "\\nint recorder_uplink_send_pending")
        self.assertIn("app_clock_valid()", helper)
        self.assertIn("uplink_fail", helper)

    def test_the_upload_waits_for_the_clock_instead_of_failing_at_once(self):
        """The board joins the network a few seconds before SNTP answers.

        The upload runs on its own worker task, so it can wait for the clock
        without freezing the UI. Failing at once would mean an upload that
        the user pressed right after boot never happens.
        """
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_clock_ready(void)", "\\nint recorder_uplink_send_pending")
        self.assertIn("while (!app_clock_valid())", helper)
        self.assertIn("vTaskDelay", helper)
        # The wait has to be bounded, or a board that never syncs keeps the
        # worker task forever.
        self.assertIn("UPLINK_CLOCK_WAIT_MS", helper)

    def test_a_missing_clock_is_reported_as_such(self):
        """The user has to see "Clock not set", not a TLS error."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_clock_ready(void)", "\\nint recorder_uplink_send_pending")
        self.assertIn("Clock not set", helper)

    def test_the_notes_sync_waits_for_the_clock(self):
        """The notes sync is a once-per-boot one-shot.

        It normally fires a few seconds before SNTP answers. Failing there
        would mean the notes never download at all, so it has to wait.
        """
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        self.assertIn("while (!app_clock_valid())", source)
        self.assertIn("NOTES_SYNC_CLOCK_WAIT_MS", source)

    def test_the_notes_download_checks_the_clock_too(self):
        """The notes sync runs the same TLS handshake."""
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        self.assertIn("app_clock_valid()", source)

    def test_the_clock_lives_in_its_own_component(self):
        """app_recorder must not depend on app_onboard.

        app_onboard already depends on app_recorder, so the clock helper
        cannot live there. A tiny component that both may depend on keeps
        the dependency direction clean.
        """
        self.assertTrue((CLOCK_ROOT / "include" / "app_clock.h").is_file())
        recorder_cmake = _read(RECORDER_ROOT / "CMakeLists.txt")
        onboard_cmake = _read(ONBOARD_ROOT / "CMakeLists.txt")
        self.assertIn("app_clock", recorder_cmake)
        self.assertIn("app_clock", onboard_cmake)
        # Neither component may reach into the other.
        self.assertNotIn("app_onboard.h", _read(RECORDER_ROOT / "recorder_uplink.c"))
        self.assertNotIn("recorder_uplink.h", _read(ONBOARD_ROOT / "app_onboard.c"))


class UplinkFailureLoggingPolicyTest(unittest.TestCase):
    """A failed upload must not be silent.

    The upload path returned -1 without a log line, so the board had an IP
    and the inbox never saw it, with no way to tell why.
    """

    def test_every_failure_goes_through_the_logging_helper(self):
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_fail(", "\\n/* The certificate bundle")
        self.assertIn("s_failure = reason;", helper)
        self.assertIn("ESP_LOGE", helper)

    def test_the_failure_reason_is_logged(self):
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_fail(", "\\n/* The certificate bundle")
        self.assertIn("upload failed", helper)

    def test_no_failure_leaves_the_reason_unset(self):
        """The old code assigned s_failure and returned -1 directly. Every
        such exit has to go through the helper now."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        body = _body(source, "int recorder_uplink_send_pending(", "\\nconst char *recorder_uplink_failure")
        # Only the reset at the top may assign s_failure in this function.
        self.assertEqual(body.count("s_failure ="), 1)
        self.assertIn("s_failure = NULL;", body)
        self.assertIn("uplink_fail(", body)

    def test_the_post_helper_logs_the_failing_step(self):
        """A rejected endpoint or a dead client has to say which step
        failed."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        post = _body(source, "static int post_request(", "\\n/* The recordings tree")
        self.assertIn("ESP_LOGE", post)
        self.assertIn("the upload endpoint is not usable", post)
        self.assertIn("the HTTP client could not be created", post)


class NotesSyncLoggingPolicyTest(unittest.TestCase):
    """The notes sync must not fail silently.

    The sync_task had no log lines at all, so a failed download looked like
    a device that was simply idle.
    """

    def test_the_module_has_a_log_tag(self):
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        self.assertIn('static const char *TAG = "', source)

    def test_the_sync_task_logs_its_start_and_its_end(self):
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        task = _body(source, "static void sync_task(void *arg)", "\\nvoid meeting_notes_sync_start")
        self.assertIn("sync task started", task)
        self.assertIn("sync ended", task)

    def test_the_status_code_is_logged(self):
        """The HTTP status that came back has to be visible."""
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        download = _body(source, "static int download(char *out, size_t cap)", "\\nstatic void sync_task")
        self.assertIn("status=", download)
        self.assertIn("esp_http_client_get_status_code", download)

    def test_the_client_error_is_logged(self):
        """The esp_err_t that the client returned has to be visible."""
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        download = _body(source, "static int download(char *out, size_t cap)", "\\nstatic void sync_task")
        self.assertIn("esp_err_to_name", download)
        self.assertIn("esp_http_client_open", download)


if __name__ == "__main__":
    unittest.main()
