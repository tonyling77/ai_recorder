import unittest
from pathlib import Path


APP_HELLO_SOURCE = Path(__file__).parent.parent / "app_hello.c"


def _source() -> str:
    return APP_HELLO_SOURCE.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class UploadStatusPillTest(unittest.TestCase):
    """The upload outcome has to be visible on the screen.

    The screen used to stay on "Uploading" even after the upload had already
    finished, so the user could not tell whether it worked. The status pill
    now carries the outcome token next to the sub-text.
    """

    def test_the_start_path_sets_the_sync_pill(self):
        source = _source()
        request = _body(source, "static void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                        "\nstatic void show_note")
        self.assertIn('"SYNC"', request)
        self.assertIn('"Uploading"', request)

    def test_the_success_path_sets_the_ok_pill(self):
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn('"OK"', tick)
        self.assertIn('"Uploaded"', tick)

    def test_the_failure_path_sets_the_err_pill(self):
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn('"ERR"', tick)

    def test_the_pill_and_the_sub_text_are_written_together(self):
        """One helper writes both, so the outcome is never only half shown."""
        source = _source()
        helper = _body(source, "static void set_upload_status(struct app_hello *state", "\nstatic esp_gsp_err_t (*const")
        self.assertIn("gsp_recorder_status_text_set_text", helper)
        self.assertIn("gsp_recorder_sub_text_set_text", helper)

    def test_the_upload_paths_use_the_helper(self):
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn('set_upload_status(state, "OK"', tick)
        self.assertIn('set_upload_status(state, "ERR"', tick)
        request = _body(source, "static void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                        "\nstatic void show_note")
        self.assertIn('set_upload_status(state, "SYNC"', request)

    def test_the_pill_tokens_are_short_uppercase(self):
        """The pill style is "REC" and "IDLE", so the upload tokens follow it."""
        source = _source()
        for token in ("SYNC", "OK", "ERR"):
            self.assertIn(f'"{token}"', source)

    def test_the_upload_text_survives_until_the_next_user_action(self):
        """A later tick must not overwrite what the completion branch set."""
        source = _source()
        # The upload owns the main sub-text once it started.
        self.assertIn("upload_status_shown", source)
        # A user action takes the screen back.
        toggle = _body(source, "static void toggle_recording(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                       "\nstatic void request_upload")
        self.assertIn("state->upload_status_shown = false;", toggle)
        request = _body(source, "static void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                        "\nstatic void show_note")
        self.assertIn("state->upload_status_shown = false;", request)
        # The other writers of the main sub-text have to respect it.
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn("if (!state->upload_status_shown)", tick)
        provision = _body(source, "static void finish_provision(struct app_hello *state)\n{",
                          "\n#if defined(ESP_PLATFORM)")
        self.assertIn("!state->upload_status_shown", provision)

    def test_the_idle_label_does_not_overwrite_the_upload_result(self):
        """The provisioning finish writes the same sub-text, so it has to
        leave the upload result alone."""
        source = _source()
        provision = _body(source, "static void finish_provision(struct app_hello *state)\n{",
                          "\n#if defined(ESP_PLATFORM)")
        self.assertNotIn('gsp_recorder_sub_text_set_text(state->ui, "Tap Record to start");', provision)


if __name__ == "__main__":
    unittest.main()
