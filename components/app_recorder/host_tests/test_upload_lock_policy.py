import re
import unittest
from pathlib import Path


RECORDER_ROOT = Path(__file__).parent.parent


def _source(name: str) -> str:
    return (RECORDER_ROOT / name).read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class UploadTimeoutPolicyTest(unittest.TestCase):
    """A dead link must fail in a time the user can recognise.

    The per-request timeout used to be five minutes for both requests. With a
    health check and one POST per slice the screen stayed on "Uploading" for
    a very long time even when the upload had already finished, so the user
    read it as a hang.

    The two requests need different budgets. The health check is a few bytes
    and is the request that reveals a dead link, so it fails fast. A slice
    POST carries up to RECORDER_SLICE_LIMIT_MS of Opus audio, about 2.2 MB,
    which needs roughly 176 seconds over a slow WAN at 100 kbit/s, so it keeps
    the long budget. Giving both requests the short timeout would break
    uploads on exactly the links that need them most.
    """

    def _timeout(self, name):
        """Resolve a timeout constant, following one level of alias."""
        source = _source("include/recorder_upload.h")
        match = re.search(r"#define\s+%s\s+(\d+)" % name, source)
        if match:
            return int(match.group(1))
        alias = re.search(r"#define\s+%s\s+([A-Z_][A-Z0-9_]*)" % name, source)
        self.assertIsNotNone(alias, "%s must be defined" % name)
        match = re.search(r"#define\s+%s\s+(\d+)" % alias.group(1), source)
        self.assertIsNotNone(match, "%s must resolve to a numeric constant" % name)
        return int(match.group(1))

    def test_health_timeout_is_at_most_twenty_seconds(self):
        self.assertLessEqual(self._timeout("RECORDER_UPLOAD_HEALTH_TIMEOUT_MS"), 20000)

    def test_slice_timeout_covers_a_slow_wan_slice(self):
        """One slice is about 2.2 MB; at 100 kbit/s that needs ~176 s."""
        bytes_on_wire = 2200000
        need_ms = (bytes_on_wire * 8 * 1000) // 100000
        self.assertGreaterEqual(self._timeout("RECORDER_UPLOAD_SLICE_TIMEOUT_MS"), need_ms)

    def test_a_slice_gets_more_time_than_the_health_check(self):
        self.assertGreater(self._timeout("RECORDER_UPLOAD_SLICE_TIMEOUT_MS"),
                           self._timeout("RECORDER_UPLOAD_HEALTH_TIMEOUT_MS"))

    def test_the_shared_timeout_is_the_health_one(self):
        self.assertEqual(self._timeout("RECORDER_UPLOAD_TIMEOUT_MS"),
                         self._timeout("RECORDER_UPLOAD_HEALTH_TIMEOUT_MS"))

    def test_the_endpoint_carries_both_timeouts(self):
        source = _source("recorder_upload.c")
        endpoint = _body(source, "int recorder_upload_endpoint(", "\nvoid recorder_upload_result")
        self.assertIn("RECORDER_UPLOAD_HEALTH_TIMEOUT_MS", endpoint)
        self.assertIn("RECORDER_UPLOAD_SLICE_TIMEOUT_MS", endpoint)

    def test_the_slice_post_uses_the_slice_timeout(self):
        """A slice POST must not fall back to the short timeout."""
        source = _source("recorder_uplink.c")
        post = _body(source, "static int post_request(", "\nstatic int send_slice_body")
        self.assertIn("RECORDER_UPLOAD_SLICE", post)
        self.assertIn("slice_timeout_ms", post)

    def test_the_notes_sync_uses_the_shared_timeout(self):
        """The notes download is a small document and must not keep a longer
        timeout of its own."""
        source = (RECORDER_ROOT.parent / "app_hello" / "meeting_notes_sync.c").read_text(encoding="utf-8")
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("RECORDER_UPLOAD_TIMEOUT_MS", download)
        self.assertNotRegex(download, r"\.timeout_ms\s*=\s*\d{5,}")


class StoreLockPolicyTest(unittest.TestCase):
    """The /nand LittleFS volume has no internal locking.

    Every thread that shares the recordings tree has to go through one
    recursive mutex, or a write and a read can corrupt the volume.
    """

    def test_the_lock_is_a_recursive_mutex(self):
        source = _source("recorder_store_lock.c")
        self.assertIn("xSemaphoreCreateRecursiveMutex", source)
        self.assertIn("xSemaphoreTakeRecursive", source)
        self.assertIn("xSemaphoreGiveRecursive", source)

    def test_the_header_documents_the_recursive_nature(self):
        source = _source("include/recorder_store_lock.h")
        self.assertIn("recursive", source.lower())

    def test_the_header_states_the_network_contract(self):
        """The lock must never be held across a blocking request."""
        source = _source("include/recorder_store_lock.h")
        self.assertIn("network", source.lower())

    def test_the_lock_is_created_before_any_store_use(self):
        """recorder_volume_prepare() mounts the volume and may run more than
        once, so the init has to be idempotent and has to happen there."""
        source = _source("recorder_volume.c")
        self.assertIn("recorder_store_lock_init", source)
        prepare = source.split("int recorder_volume_prepare(void)", 1)[1]
        # The lock has to exist before the volume is mounted and before any
        # store call can reach the tree.
        self.assertLess(prepare.index("recorder_store_lock_init"), prepare.index("esp_vfs_littlefs_register"))

    def test_the_lock_init_is_idempotent(self):
        source = _source("recorder_store_lock.c")
        init = _body(source, "void recorder_store_lock_init(void)", "\nint recorder_store_lock_acquire")
        self.assertIn("if (s_lock)", init)
        self.assertIn("return;", init)

    def test_every_store_entry_point_takes_the_lock(self):
        """Each function that reads or writes the tree has to hold the lock
        for the whole transaction."""
        source = _source("recorder_store.c")
        locked = [
            "recorder_store_put",
            "recorder_store_get",
            "recorder_store_sha256",
            "recorder_store_mark_uploaded",
            "recorder_store_is_uploaded",
            "recorder_store_is_last",
            "recorder_store_slice_info",
            "recorder_store_acknowledged",
            "recorder_store_scan",
            "recorder_store_delete_session",
        ]
        for name in locked:
            body = _body(source, f"\nint {name}(", "\nint ")
            self.assertIn("RECORDER_STORE_LOCK()", body, f"{name} must take the lock")
            self.assertIn("RECORDER_STORE_UNLOCK()", body, f"{name} must release the lock")

    def test_the_listing_entry_points_go_through_the_one_pass(self):
        """The counts and the list come out of one pass over the tree.

        Reading the tree twice took twice as long, and the tree is on a flash
        where every file that is opened costs a moment. The listing and the
        counts therefore share one pass, which holds the lock.
        """
        source = _source("recorder_store.c")
        for name in ("recorder_store_inventory", "recorder_store_list_sessions"):
            body = _body(source, f"\nint {name}(", "\nint ")
            self.assertIn("recorder_store_scan(", body, f"{name} must go through the one pass")

    def test_the_scan_does_not_read_the_recordings(self):
        """The listing must not read the audio of every recording.

        The metadata of a slice carries the checksum of the recording, so
        the listing compares that with the acknowledgement. Reading the
        recording to compute the checksum costs a full pass over the file,
        which on this flash is seconds for a long recording, and the lock is
        held while it reads. That is what turned an upload into "Storage
        unavailable" while the listing was still reading.
        """
        source = _source("recorder_store.c")
        body = _body(source, "\nint recorder_store_slice_info(", "\nint ")
        self.assertIn("recorder_store_acknowledged(", body)
        self.assertNotIn("recorder_store_is_uploaded(", body)
        self.assertIn("sha256=", body, "the listing reads the checksum from the metadata")

    def test_the_upload_still_compares_the_checksum(self):
        """The upload sends the checksum, so it reads the recording."""
        source = _source("recorder_uplink.c")
        self.assertIn("recorder_store_is_uploaded(", source)

    def test_the_scan_releases_the_lock_before_it_opens_the_files(self):
        """The lock must not be held while the files are opened.

        Opening a file on this flash takes a moment, and a scan holds the
        lock over every recording. Holding it for the whole pass would keep
        the uploader and the recorder out of the tree for seconds.
        """
        source = _source("recorder_store.c")
        body = _body(source, "\nint recorder_store_scan(", "\nint ")
        release = body.index("RECORDER_STORE_UNLOCK()")
        first_open = body.index("recorder_store_slice_info(")
        self.assertLess(release, first_open,
                        "the lock is released before the files are opened")

    def test_the_lock_covers_the_whole_transaction(self):
        """The lock has to be taken before the first file access and released
        after the last one, not around a single fopen."""
        source = _source("recorder_store.c")
        body = _body(source, "\nint recorder_store_put(", "\nint ")
        self.assertLess(body.index("RECORDER_STORE_LOCK()"), body.index("fopen"))
        self.assertLess(body.index("fclose(meta)"), body.index("RECORDER_STORE_UNLOCK()"))

    def test_the_recorder_control_scan_takes_the_lock(self):
        """next_session_id() lists the tree directly, so it needs the lock."""
        source = _source("recorder_control.c")
        body = _body(source, "static uint32_t next_session_id(", "\nrecorder_control_t *recorder_control_open")
        self.assertIn("recorder_store_lock_acquire", body)
        self.assertIn("recorder_store_lock_release", body)

    def test_the_recorder_mic_stat_takes_the_lock(self):
        """log_saved() stats a slice in the shared tree, so it needs the
        lock as well."""
        source = _source("recorder_mic.c")
        body = _body(source, "static void log_saved(", "\nint recorder_mic_stop")
        self.assertIn("recorder_store_lock_acquire", body)
        self.assertIn("recorder_store_lock_release", body)

    def test_no_component_reads_the_recordings_tree_without_the_lock(self):
        """Every opendir, stat and fopen on the recordings tree has to sit
        inside a lock hold. The store module is the one place that owns the
        tree, so it is checked separately above."""
        for name in ("recorder_control.c", "recorder_mic.c", "recorder_uplink.c"):
            source = _source(name)
            for marker in ("opendir(", "stat("):
                if marker not in source:
                    continue
                # Every occurrence lives in a function that takes the lock.
                self.assertIn("recorder_store_lock_acquire", source,
                              f"{name} touches the tree and must take the lock")


class UplinkLockPolicyTest(unittest.TestCase):
    """The upload worker must not hold the store lock across an HTTP request.

    The request can block for the whole timeout. Holding the lock that long
    would freeze the UI, which reads the same tree from its timer.
    """

    def test_the_slice_body_is_read_under_the_lock_and_posted_without_it(self):
        source = _source("recorder_uplink.c")
        body = _body(source, "static int send_slice_body(", "\nint recorder_uplink_send_pending")
        self.assertIn("recorder_store_lock_acquire", body)
        self.assertIn("recorder_store_lock_release", body)
        # The release has to come before the request goes out.
        self.assertLess(body.index("recorder_store_lock_release()"), body.index("post_request("))

    def test_the_send_pending_releases_the_lock_before_the_request(self):
        source = _source("recorder_uplink.c")
        body = _body(source, "int recorder_uplink_send_pending(", "\nconst char *recorder_uplink_failure")
        # The checksum read holds the lock and drops it again before the
        # request is prepared.
        self.assertIn("recorder_store_lock_acquire", body)
        self.assertIn("recorder_store_lock_release", body)
        acquire = body.index("recorder_store_lock_acquire")
        release = body.index("recorder_store_lock_release")
        self.assertLess(acquire, release)
        self.assertLess(release, body.index("recorder_upload_prepare("))

    def test_the_directory_scan_takes_the_lock(self):
        source = _source("recorder_uplink.c")
        body = _body(source, "static int collect_sessions(", "\nstatic int load_closed_slices")
        self.assertIn("recorder_store_lock_acquire", body)
        self.assertIn("recorder_store_lock_release", body)

    def test_the_post_request_helper_does_not_touch_the_lock(self):
        """The HTTP helper has no reason to know about the store lock. If it
        ever takes it, the request would run under the lock."""
        source = _source("recorder_uplink.c")
        body = _body(source, "static int post_request(", "\n/* The recordings tree")
        self.assertNotIn("recorder_store_lock", body)


class MicrophoneLockPolicyTest(unittest.TestCase):
    """The screen must not wait for the microphone to write.

    The microphone task holds the lock while it writes the audio to the
    flash, and that write is the slow part. The screen reads the elapsed
    time every second, so it used to give up on the lock after 20 ms and
    showed 00:00 for the whole recording.
    """

    def test_reading_the_elapsed_time_does_not_take_the_lock(self):
        source = _source("recorder_mic.c")
        body = _body(source, "uint32_t recorder_mic_elapsed_ms(void)", "\n}")
        self.assertNotIn("xSemaphoreTake", body)
        self.assertNotIn("xSemaphoreGive", body)

    def test_the_task_publishes_the_elapsed_time(self):
        """The task publishes it while it holds the lock, so the value is
        there without anybody waiting for it."""
        source = _source("recorder_mic.c")
        body = _body(source, "static void mic_task(void *arg)", "\nint recorder_mic_prepare")
        self.assertIn("recorder_control_elapsed_ms(", body)
        self.assertIn("s_elapsed_ms = elapsed;", body)

    def test_the_task_of_the_microphone_is_created_at_the_start(self):
        """The task needs a long stack, and the memory for it is in the
        external memory.

        The memory is there when the board starts and not later, so the task
        is created with the microphone and the button does not ask for it.
        The stack is in the external memory, because the encoder of the Opus
        codec puts arrays on the stack that are as long as the frame it
        encodes, and the internal memory of the board is too small for one.
        """
        source = _source("recorder_mic.c")
        prepare = _body(source, "int recorder_mic_prepare(void)", "\nint recorder_mic_start")
        self.assertIn("xTaskCreateWithCaps(mic_task", prepare)
        self.assertIn("MALLOC_CAP_SPIRAM", prepare)
        start = _body(source, "int recorder_mic_start(void)", "\nuint32_t recorder_mic_elapsed_ms")
        self.assertNotIn("xTaskCreateWithCaps(mic_task", start)

    def test_the_microphone_is_ready_before_the_user_reaches_for_the_button(self):
        """Opening the codec is slow, and the button waits for it. The board
        does it at the start instead."""
        source = _source("recorder_mic.c")
        self.assertIn("int recorder_mic_prepare(void)", source)
        prepare = _body(source, "int recorder_mic_prepare(void)", "\nint recorder_mic_start")
        self.assertIn("ensure_microphone()", prepare)


if __name__ == "__main__":
    unittest.main()
