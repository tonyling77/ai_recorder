import re
import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
UPLOAD_SOURCE = COMPONENT / "xiaozhi_upload.c"
BIND_SOURCE = COMPONENT / "xiaozhi_bind.c"
NOTES_SOURCE = COMPONENT / "xiaozhi_notes.c"


def _source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


def _block(text: str, start: str) -> str:
    """Return the block that starts at the marker, up to its closing brace.

    The braces are matched, so a block that holds another block is returned
    whole. A marker without a block behind it returns the rest of the text.
    """
    tail = text.split(start, 1)[1]
    depth = 1
    for index, char in enumerate(tail):
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return tail[:index]
    return tail


def _log_calls(text: str) -> list:
    """Return the argument text of every ESP_LOG call in the slice."""
    return re.findall(r"ESP_LOGI\(TAG,.*?\);", text, re.DOTALL)


class XiaozhiUploadLoggingPolicyTest(unittest.TestCase):
    """The upload path must show what the server replied.

    The Xiaozhi upload had almost no logging. When the upload failed the user
    only saw a screen string like "No card came back", and there was no way
    to tell whether the server had refused the audio, answered with another
    shape, or never answered at all. The device log has to carry the server
    reply, so the open question (does the server accept a meeting's audio as
    a realtime stream and return a card in the requested fields?) can be
    answered from the log.
    """

    def test_the_websocket_event_handler_logs_the_server_message(self):
        source = _source(UPLOAD_SOURCE)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        self.assertIn("ESP_LOGI", handler)
        self.assertIn("Xiaozhi message type %s state %s text", handler)

    def test_the_message_log_carries_the_type_the_state_and_the_text(self):
        source = _source(UPLOAD_SOURCE)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        # The text is the payload that matters, so the log has to hold it.
        self.assertRegex(
            handler,
            r"ESP_LOGI\(TAG,\s*\"Xiaozhi message type %s state %s text %\.\*s\"",
        )
        self.assertIn("message.text", handler)

    def test_the_message_log_is_truncated(self):
        """A long answer must not flood the log, so the text is cut."""
        source = _source(UPLOAD_SOURCE)
        self.assertIn("#define XIAOZHI_UPLOAD_LOG_TEXT_CAP 512", source)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        self.assertIn("XIAOZHI_UPLOAD_LOG_TEXT_CAP", handler)

    def test_the_stt_text_is_not_logged(self):
        """The user's own speech must never reach the log.

        The stt frame carries the transcript of the meeting. The device keeps
        it in memory for the length of one upload, because the card is asked
        for with the transcript, but the words the meeting said must not be
        written to the serial log. The log carries the length of the text, so
        the size of the meeting is visible without its content.
        """
        source = _source(UPLOAD_SOURCE)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        stt = _block(handler, 'if (strcmp(message.type, "stt") == 0) {')
        self.assertTrue(_log_calls(stt), "the stt branch logs the frame")
        for call in _log_calls(stt):
            self.assertNotIn("message.text", call)
            self.assertNotIn("transcript,", call)
        # The frame is still visible in the log.
        self.assertIn("message.type", stt)
        self.assertIn("message.state", stt)

    def test_the_stt_text_is_not_written_to_a_file(self):
        """The transcript stays in memory. It is never written down.

        The meeting content belongs on the device as a card, not as a
        transcript file. A transcript file would also be a copy of what
        people said, and the log and the flash must not carry it.
        """
        source = _source(UPLOAD_SOURCE)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        stt = _block(handler, 'if (strcmp(message.type, "stt") == 0) {')
        for call in ("fopen", "fwrite", "open(", "write(", "lfs_file", "esp_vfs"):
            self.assertNotIn(call, stt)

    def test_the_stt_branch_comes_before_the_message_log(self):
        """The stt branch returns, so the text log cannot reach it.

        The hello branch logs its own frame and returns. The general text
        log sits after the stt branch, so the stt frames can never reach it.
        """
        source = _source(UPLOAD_SOURCE)
        handler = _body(source, "static void on_websocket_event(", "\nstatic bool wait_for_flag")
        stt_at = handler.index('strcmp(message.type, "stt")')
        # The general log is the one that follows the stt branch, so it is
        # the last of the two "text" logs.
        text_logs = [m.start() for m in re.finditer(r"Xiaozhi message type %s state %s text", handler)]
        self.assertEqual(len(text_logs), 2)
        hello_at, general_at = text_logs
        self.assertLess(hello_at, stt_at)
        self.assertLess(stt_at, general_at)

    def test_the_connect_is_logged_without_the_token(self):
        """The endpoint is logged, the token is not."""
        source = _source(UPLOAD_SOURCE)
        connect = _body(source, "esp_websocket_client_start(client)", "\n    }")
        self.assertIn("Xiaozhi websocket started", connect)
        self.assertIn("request->url", connect)
        self.assertIn("request->device_id", connect)
        self.assertNotIn("request->token", connect)
        self.assertNotIn("authorization", connect)

    def test_the_server_hello_session_is_logged(self):
        source = _source(UPLOAD_SOURCE)
        self.assertIn("Xiaozhi server hello session", source)

    def test_the_audio_progress_is_logged_every_n_packets(self):
        """The audio progress is logged so a stalled upload is visible."""
        source = _source(UPLOAD_SOURCE)
        self.assertIn("#define XIAOZHI_UPLOAD_PROGRESS_PACKETS 50u", source)
        slice_fn = _body(source, "static int send_slice(", "\nstatic void upload_task")
        self.assertIn("packets % XIAOZHI_UPLOAD_PROGRESS_PACKETS == 0", slice_fn)
        self.assertIn("Xiaozhi audio slice", slice_fn)

    def test_every_slice_reports_its_own_result(self):
        """Each slice reports its index, its duration and its bytes."""
        source = _source(UPLOAD_SOURCE)
        task = _body(source, "static void upload_task(", "\nstatic uint32_t xiaozhi_upload_next_session")
        self.assertIn("Xiaozhi audio slice %u done", task)
        self.assertIn("duration_ms", task)
        self.assertIn("slice_bytes", task)

    def test_the_card_decision_is_logged(self):
        """The assembled reply and the verdict show why a card was refused."""
        source = _source(UPLOAD_SOURCE)
        task = _body(source, "static void upload_task(", "\nstatic uint32_t xiaozhi_upload_next_session")
        self.assertIn("Xiaozhi card reply", task)
        self.assertIn("complete %d", task)

    def test_the_final_outcome_is_logged(self):
        """The result says whether the upload worked and why it did not."""
        source = _source(UPLOAD_SOURCE)
        finish = _body(source, "finish:", "\n    if (client) {")
        self.assertIn("Xiaozhi upload failed", finish)
        self.assertIn("result.failure", finish)
        self.assertIn("Xiaozhi upload succeeded", finish)

    def test_the_existing_log_lines_are_still_there(self):
        """The new lines are added, the old ones are kept."""
        source = _source(UPLOAD_SOURCE)
        self.assertIn('ESP_LOGI(TAG, "Xiaozhi upload started")', source)
        self.assertIn('ESP_LOGE(TAG, "Xiaozhi upload task was not created")', source)


class XiaozhiBindLoggingPolicyTest(unittest.TestCase):
    """The activation reply must be readable from the log.

    The activation reply carries the websocket token, so the reply may only
    be logged with that value redacted. The token itself is the device's
    credential and must not end up in a log file.
    """

    def test_the_redaction_helper_exists(self):
        source = _source(BIND_SOURCE)
        self.assertIn("static void log_reply(const char *what, const char *body)", source)

    def test_the_helper_redacts_every_credential(self):
        """Every credential in the reply is redacted.

        The reply carries a websocket token and the MQTT username and
        password. This project does not use MQTT, but a credential that lands
        in a log file is a credential that leaked, so every one of them is
        replaced. The search is not tied to a block of the reply, so a
        credential that moves is still redacted.
        """
        source = _source(BIND_SOURCE)
        helper = _body(source, "static void log_reply(const char *what, const char *body)", "\nstatic int post_json")
        for key in ('"\\"token\\""', '"\\"username\\""', '"\\"password\\""'):
            self.assertIn(key, helper)
        # The value that replaces a credential is three stars.
        self.assertIn('"***"', source)

    def test_the_helper_works_on_a_copy(self):
        """The body that goes to the parser keeps its token."""
        source = _source(BIND_SOURCE)
        helper = _body(source, "static void log_reply(const char *what, const char *body)", "\nstatic int post_json")
        self.assertIn("char copy[512]", helper)
        self.assertIn("memcpy(copy, body, len)", helper)
        # The copy is the one that is written.
        self.assertIn("ESP_LOGI(TAG, \"Xiaozhi %s: %s\", what, copy)", helper)

    def test_the_copy_is_at_most_512_bytes(self):
        source = _source(BIND_SOURCE)
        helper = _body(source, "static void log_reply(const char *what, const char *body)", "\nstatic int post_json")
        self.assertRegex(helper, r"char copy\[512\]")

    def test_the_reply_log_goes_through_the_helper(self):
        """The reply is never logged directly, only through the redactor."""
        source = _source(BIND_SOURCE)
        task = _body(source, "static void bind_task(", "\nvoid xiaozhi_bind_start")
        self.assertIn('log_reply("OTA reply", body)', task)
        self.assertIn('log_reply("activate reply", scratch)', task)
        # A raw body would leak the token.
        self.assertNotIn('ESP_LOGI(TAG, "Xiaozhi OTA reply: %s", body)', task)

    def test_the_identity_headers_are_logged(self):
        """The device id and the client id confirm which device checked in."""
        source = _source(BIND_SOURCE)
        post = _body(source, "static int post_json(", "\nstatic void offer_body")
        self.assertIn("Device-Id", post)
        self.assertIn("Client-Id", post)
        self.assertIn("Activation-Version", post)
        self.assertIn("device_id", post)
        self.assertIn("client_id", post)

    def test_the_existing_log_lines_are_still_there(self):
        source = _source(BIND_SOURCE)
        self.assertIn('ESP_LOGI(TAG, "Xiaozhi OTA status %d code %d"', source)
        self.assertIn('ESP_LOGI(TAG, "Xiaozhi activate status %d"', source)


class XiaozhiNotesLoggingPolicyTest(unittest.TestCase):
    """The card is the answer to the meeting, so the device asks nothing.

    The device used to send a second message to ask for the card. The text
    of a detect message is the wake word on the official cloud, so the
    server answered with an empty alert and then stayed silent for a
    minute: no card came back, and the device had become a conversation
    instead of a recorder.
    """

    def test_the_device_asks_nothing(self):
        source = _source(NOTES_SOURCE)
        self.assertNotIn("xiaozhi_note_card_request", source)

    def test_the_upload_sends_no_question(self):
        """Only the audio goes up. The answer to it is the card."""
        source = _source(UPLOAD_SOURCE)
        self.assertNotIn("xiaozhi_note_card_request", source)
        self.assertNotIn("send_text(client, ask)", source)

    def test_the_answer_to_the_meeting_is_read_as_the_card(self):
        """The reply that came with the meeting is not thrown away.

        The reply is prepared once, when the upload starts. The device used
        to prepare it a second time, after the meeting, so that only the
        answer to its question was read as the card.
        """
        source = _source(UPLOAD_SOURCE)
        self.assertEqual(source.count("xiaozhi_note_reply_init(&s_messages->reply)"), 1,
                         "the reply is prepared once, at the start of the upload")
        self.assertIn("xiaozhi_note_reply_finish(&spoken, notes)", source)

    def test_the_card_is_parsed_from_the_answer(self):
        source = _source(NOTES_SOURCE)
        self.assertIn("has_line(text, \"id \")", source)
        self.assertIn("has_line(text, \"title \")", source)


if __name__ == "__main__":
    unittest.main()
