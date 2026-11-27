import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
APP_HELLO_SOURCE = COMPONENT / "app_hello.c"
BIND_SOURCE = COMPONENT / "xiaozhi_bind.c"
SYNC_SOURCE = COMPONENT / "meeting_notes_sync.c"


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class ChannelReleasePolicyTest(unittest.TestCase):
    """Switching the channel has to let go of what the old channel holds.

    The two channels are a choice, so one of them is on and the other one is
    off. The board does not have the memory for both: the pull of the
    minutes holds sixteen kilobytes of stack in the internal memory and
    eight kilobytes of body, and the check-in of the cloud holds a socket
    and a buffer while it is talking to the public server.

    So leaving a channel has to end the work of that channel before the
    work of the new one starts. The work of a channel is started and ended
    by one task, the task of the channel, because letting the work of a
    channel end takes a while and the screen must keep answering.
    """

    def test_the_screen_does_not_start_the_work_of_a_channel(self):
        """The task of the screen only tells the task of the channel."""
        source = _source(APP_HELLO_SOURCE)
        tick = _body(source, "static void upload_tick(", "\nstatic void ")
        self.assertNotIn("meeting_notes_sync_start(", tick,
                         "the pull of the minutes belongs to the task of the channel")
        self.assertNotIn("xiaozhi_bind_start(", tick,
                         "the check-in of the cloud belongs to the task of the channel")

    def test_the_task_of_the_channel_ends_the_old_work_before_it_starts_the_new(self):
        """The old work ends first, and the wait for it is real."""
        source = _source(APP_HELLO_SOURCE)
        task = _body(source, "static void channel_task(", "\n#endif")
        self.assertIn("xiaozhi_bind_stop(", task,
                      "the check-in of the cloud has to end")
        self.assertIn("meeting_notes_sync_stop(", task,
                      "the pull of the minutes has to end")
        stop = task.split("if (current == UPLINK_CHANNEL_XIAOZHI) {", 1)[1]
        stop = stop.split("current = next;", 1)[0]
        self.assertIn("xiaozhi_bind_stop(", stop,
                      "the work of the old channel ends before the new one starts")

    def test_the_switch_asks_the_task_of_the_channel(self):
        """The switch does not do the work itself."""
        source = _source(APP_HELLO_SOURCE)
        switch = _body(source, "static void switch_channel(struct app_hello *state, uplink_channel_t channel)\n{",
                       "\n/* Show which channel")
        self.assertIn("request_channel(", switch,
                      "the switch has to ask the task of the channel")
        self.assertNotIn("xiaozhi_bind_start(", switch,
                         "the screen must not start the check-in of the cloud")
        self.assertNotIn("xiaozhi_bind_stop(", switch,
                         "the screen must not end the check-in of the cloud")

    def test_the_stop_of_the_check_in_waits_for_the_task(self):
        """The memory of the check-in is free before the other channel starts."""
        source = _source(BIND_SOURCE)
        stop = _body(source, "void xiaozhi_bind_stop(void)", "\nchar *xiaozhi_bind_take_body(")
        self.assertIn("s_stop = true;", stop, "the task has to be asked to end")
        self.assertIn("s_task", stop, "the stop has to wait for the task")
        self.assertIn("vTaskDelay(", stop, "the wait has to let the task run")

    def test_the_stop_of_the_pull_waits_for_the_task(self):
        """The memory of the pull is free before the other channel starts."""
        source = _source(SYNC_SOURCE)
        stop = _body(source, "void meeting_notes_sync_stop(void)", "\nchar *meeting_notes_sync_take(")
        self.assertIn("s_stop = true;", stop, "the task has to be asked to end")
        self.assertIn("s_sync_task", stop, "the stop has to wait for the task")
        self.assertIn("vTaskDelay(", stop, "the wait has to let the task run")

    def test_a_start_does_not_bring_a_stopping_task_back(self):
        """The flag of the stop belongs to the task that is running.

        The flag was cleared before the running task was looked at, so a
        check-in that was asked to end kept running: the cloud was asked
        for the device while the other channel was in use, and the second
        check-in ran next to it.
        """
        for path, marker, stop_flag in (
            (BIND_SOURCE, "void xiaozhi_bind_start(void)", "s_stop = false;"),
            (SYNC_SOURCE, "void meeting_notes_sync_start(void)", "s_stop = false;"),
        ):
            source = _source(path)
            start = _body(source, marker, "\nvoid ")
            clears = start.find(stop_flag)
            self.assertNotEqual(clears, -1, f"{path.name} must clear the flag for a new task")
            # The flag is cleared after the check for a running task, so a
            # task that is still ending is not brought back to life.
            self.assertLess(start.find("if (s_task)"), clears,
                            f"{path.name} must not clear the flag while a task is running")
            self.assertLess(start.find("if (s_sync_task)"), clears,
                            f"{path.name} must not clear the flag while a task is running")

    def test_the_pull_gives_up_when_the_channel_changes(self):
        """The pull ends even while it waits for the clock."""
        source = _source(SYNC_SOURCE)
        clock = _body(source, "static int wait_for_clock(void)", "\nstatic int download(")
        self.assertIn("s_stop", clock,
                      "the wait for the clock has to end when the channel changes")
        task = _body(source, "static void sync_task(void *arg)", "\nvoid meeting_notes_sync_start(")
        self.assertIn("s_stop", task,
                      "the pull has to end when the channel changes")


if __name__ == "__main__":
    unittest.main()
