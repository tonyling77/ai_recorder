import re
import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
APP_RECORDER = COMPONENT.parent / "app_recorder"
APP_HELLO_SOURCE = COMPONENT / "app_hello.c"
MAIN_SOURCE = COMPONENT.parent.parent / "main" / "main.c"
UPLINK_SOURCE = COMPONENT / "uplink_channel.c"
INBOX_SOURCE = APP_RECORDER / "recorder_inbox.c"
FLASH_SOURCE = APP_RECORDER / "recorder_flash.c"


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class FlashWritePolicyTest(unittest.TestCase):
    """The interface must not write the flash itself.

    The stack of the render task is in the external memory, and the flash is
    written with the cache of the flash turned off. The external memory is
    out of reach while that cache is off, so the writing framework refuses
    a write from a task whose stack is there: the board stops with

        assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
        cache_utils.c:114 (esp_task_stack_is_sane_cache_disabled())

    The channel was written from the settings page, which runs on the
    render task, so switching the channel took the board down. The write
    belongs to a task of its own, whose stack is in the internal memory.

    The writing of the recordings is a different thing: that goes to the
    external flash over its own bus, which does not turn the cache of the
    internal flash off.
    """

    def test_the_write_of_the_flash_runs_on_a_task_of_its_own(self):
        """The helper gives the write a stack in the internal memory."""
        source = _source(FLASH_SOURCE)
        self.assertIn("xTaskCreate(", source,
                      "the write must run on a task of its own")
        # The plain call puts the stack in the internal memory. The one that
        # allows the external memory is the one that breaks the write.
        self.assertNotIn("xTaskCreateWithCaps(", source,
                         "the stack of the write must be in the internal memory")

    def test_the_channel_is_written_through_the_helper(self):
        """The channel goes through the helper, not straight to the flash."""
        source = _source(UPLINK_SOURCE)
        self.assertIn("recorder_flash_write(", source,
                      "the channel must be written through the helper")
        # The commit is inside the callback, which runs on the task of the
        # helper. The call of the store itself must not commit.
        store = _body(source, "int uplink_channel_store(", "\nstatic const char *activation_code")
        self.assertNotIn("nvs_commit(", store,
                         "the store must not commit the flash by itself")

    def test_the_inbox_address_is_written_through_the_helper(self):
        """The address of the inbox goes through the helper as well.

        The address is confirmed from the settings page too, which runs on
        the render task.
        """
        source = _source(INBOX_SOURCE)
        self.assertIn("recorder_flash_write(", source,
                      "the address must be written through the helper")

    def test_the_switch_of_the_channel_does_not_write_the_flash_itself(self):
        """The switch hands the write over."""
        source = _source(APP_HELLO_SOURCE)
        body = _body(source, "static void switch_channel(struct app_hello *state, uplink_channel_t channel)\n{",
                     "\n/* Show which channel")
        self.assertIn("uplink_channel_store(", body)
        self.assertNotIn("nvs_", body, "the switch must not touch the flash")

    def test_the_switch_is_done_by_the_task_of_the_screen(self):
        """The switch is done where the other actions of the screen are.

        The screen is not to be touched from two tasks at once. The actions
        of the interface are done by the task of the screen, and a caller
        on another task puts the request in the queue.

        It also matters that the switch is done by the task of the screen:
        that is the task whose stack is in the external memory, so it is the
        task that could not write the flash.
        """
        source = _source(APP_HELLO_SOURCE)
        tick = _body(source, "static void upload_tick(", "\nstatic void ")
        self.assertIn("APP_HELLO_COMMAND_CHANNEL", tick,
                      "the switch must be done by the task of the screen")
        public = _body(source, "esp_gsp_err_t app_hello_switch_channel(",
                        "\nesp_gsp_err_t app_hello_request(")
        # The part that runs on the board puts the switch in the queue. The
        # simulator has no tasks, so it does the switch right away.
        board = public.split("#else", 1)[0]
        self.assertNotIn("switch_channel(", board,
                         "the caller must not do the switch by itself")
        self.assertIn("APP_HELLO_COMMAND_CHANNEL", board,
                      "the caller must put the switch in the queue")

    def test_the_stack_of_the_render_task_is_in_the_external_memory(self):
        """The reason for the rule, so it is not removed by accident.

        The stack of the render task is in the external memory because the
        internal memory is too small for it: the codec and the Wi-Fi need
        large blocks of their own. That is why the render task cannot write
        the flash.
        """
        source = _source(MAIN_SOURCE)
        self.assertIn(
            "ESP_GSP_FIELD_RENDER_TASK_STACK_PSRAM",
            source,
            "the render task keeps its stack in the external memory",
        )


if __name__ == "__main__":
    unittest.main()
