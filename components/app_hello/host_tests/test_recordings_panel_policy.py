import json
import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
APP_HELLO_SOURCE = COMPONENT / "app_hello.c"
SCENE = COMPONENT / "ui" / "main.json"
SETTINGS_SCENE = COMPONENT / "ui" / "settings.json"
UPLINK_SOURCE = COMPONENT.parent / "app_recorder" / "recorder_uplink.c"


def _source() -> str:
    return APP_HELLO_SOURCE.read_text(encoding="utf-8")


def _uplink() -> str:
    return UPLINK_SOURCE.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


def _scene() -> dict:
    return json.loads(SCENE.read_text(encoding="utf-8"))


def _settings_scene() -> dict:
    return json.loads(SETTINGS_SCENE.read_text(encoding="utf-8"))


def _object(scene: dict, name: str) -> dict:
    for entry in scene["objects"]:
        if entry.get("name") == name:
            return entry
    raise AssertionError(f"the scene has no object named {name}")


def _platform_guarded(text: str, marker: str) -> bool:
    """Return whether the line holding `marker` sits inside the device guard."""
    depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#if defined(ESP_PLATFORM)"):
            depth += 1
        elif stripped.startswith("#endif") and depth:
            depth -= 1
        if marker in line:
            return depth > 0
    raise AssertionError(f"the source has no line holding {marker}")


class RecordingsPanelUploadTest(unittest.TestCase):
    """The recordings panel has to be able to upload what the user picked.

    The panel could list the recordings, pick one or many of them and delete
    them, but there was no way to upload a recording the user picked. The
    upload was only reachable from the main screen, and it always sent
    everything that was pending.
    """

    def test_the_panel_has_an_upload_button(self):
        scene = _scene()
        buttons = [o for o in scene["objects"] if o.get("name") == "btn_files_upload"]
        self.assertEqual(len(buttons), 1, "the panel has one upload button")
        self.assertEqual(buttons[0].get("callback"), "on_files_upload")
        self.assertEqual(buttons[0].get("parent"), 20, "the button is in the recordings panel")

    def test_the_panel_buttons_share_one_row(self):
        """Four buttons have to fit the width of the panel."""
        scene = _scene()
        panel = [o for o in scene["objects"] if o.get("name") == "files_panel"][0]
        names = ("btn_files_multi", "btn_files_upload", "btn_files_delete", "btn_files_back")
        row = []
        for name in names:
            button = [o for o in scene["objects"] if o.get("name") == name][0]
            self.assertEqual(button.get("parent"), 20, f"{name} is in the panel")
            self.assertEqual(button.get("y"), 380, f"{name} is on the button row")
            row.append((button["x"], button["x"] + button["w"]))
        row.sort()
        for index in range(1, len(row)):
            self.assertLessEqual(row[index - 1][1], row[index][0],
                                 "the buttons on the row do not overlap")
        self.assertGreaterEqual(row[-1][1], panel["w"] - 28, "the last button reaches the right edge")

    def test_the_handler_sends_the_picked_recordings(self):
        source = _source()
        handler = _body(source, "gsp_recorder_event_is_on_files_upload(event)) {",
                        "} else if (gsp_recorder_event_is_on_files_delete(event)) {")
        # Only the rows the user picked are sent.
        self.assertIn("row->selected", handler)
        self.assertIn("row->open", handler)
        self.assertIn("request_upload_sessions(ui, state, sessions, count)", handler)

    def test_the_handler_says_so_when_nothing_is_picked(self):
        source = _source()
        handler = _body(source, "gsp_recorder_event_is_on_files_upload(event)) {",
                        "} else if (gsp_recorder_event_is_on_files_delete(event)) {")
        self.assertIn("Select a saved file", handler)

    def test_the_main_screen_still_uploads_everything(self):
        """The main screen has no selection, so it keeps its old meaning."""
        source = _source()
        request = _body(source, "static void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                        "\nstatic void request_upload_sessions")
        self.assertIn("state->upload_session_count = 0;", request)
        self.assertIn("start_upload(ui, state);", request)

    def test_the_private_upload_only_sends_the_picked_recordings(self):
        """The upload takes a list and leaves the other recordings alone."""
        uplink = _uplink()
        self.assertIn("int recorder_uplink_send_pending(const char *root, const char *device_id, const uint32_t *only,",
                      uplink)
        body = _body(uplink, "int recorder_uplink_send_pending(", "\nconst char *recorder_uplink_failure")
        self.assertIn("session_in_list(only, only_count, sessions[session_i])", body)
        # A recording that is not picked is skipped, not uploaded.
        self.assertIn("continue;", body)

    def test_the_worker_passes_the_selection(self):
        source = _source()
        worker = _body(source, "static void upload_worker(void *ctx)\n{", "\nstatic void toggle_recording")
        self.assertIn("recorder_uplink_send_pending", worker)
        self.assertIn("state->upload_sessions", worker)

    def test_the_xiaozhi_queue_goes_on_with_the_next_recording(self):
        """The Xiaozhi upload carries one meeting per connection, so the
        next picked recording goes out when the first one is done."""
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn("state->upload_session_pos++", tick)
        self.assertIn("xiaozhi_queue_wait", tick)

    def test_the_queue_wait_is_bounded(self):
        """A task that never ends must not hold the queue forever."""
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn("APP_HELLO_XIAOZHI_QUEUE_TRIES", tick)
        self.assertIn("state->xiaozhi_queue_tries >= APP_HELLO_XIAOZHI_QUEUE_TRIES", tick)


class RecordingsPanelOpeningTest(unittest.TestCase):
    """Opening the panel has to feel immediate.

    The panel used to be bound to the application the first time it was
    opened, and the recordings were read before the panel appeared. The
    screen did not answer the tap until both were done, and the user felt
    the screen hang.
    """

    def test_the_panel_needs_no_binding_before_it_opens(self):
        """The rows are buttons, so opening the panel costs nothing."""
        source = _source()
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertNotIn("bind_file_list", create)

    def test_the_panel_is_shown_before_the_recordings_are_read(self):
        source = _source()
        handler = _body(source, "gsp_recorder_event_is_on_files(event) || gsp_recorder_event_is_on_files_back(event)) {",
                        "} else if (gsp_recorder_event_is_on_files_multi(event)) {")
        self.assertLess(handler.index("set_files_visible(state, true)"),
                        handler.index("reload_file_rows(state)"))

    def test_the_screen_does_not_read_the_tree_itself(self):
        """Reading the tree takes seconds, so it runs on its own task.

        The screen used to read the tree itself, which left the application
        blocked for the better part of a minute after every start and left
        the panel unresponsive for the same time every time it was opened.
        """
        source = _source()
        handler = _body(source, "gsp_recorder_event_is_on_files(event) || gsp_recorder_event_is_on_files_back(event)) {",
                        "} else if (gsp_recorder_event_is_on_files_multi(event)) {")
        self.assertIn("files_scan_request(state);", handler)
        self.assertNotIn("recorder_store_open", handler)
        self.assertNotIn("recorder_store_list_sessions", handler)

    def test_the_read_runs_on_its_own_task(self):
        source = _source()
        self.assertIn("static void files_scan_task(void *arg)", source)
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertIn("files_scan_task", create)

    def test_the_read_runs_below_the_screen(self):
        """The read is background work and must not take the CPU from the
        screen.

        At the same priority as the screen, the read starved it: the display
        needed 453 ms for a frame and reported that it could not find a
        window to draw in, so the screen stopped repainting while the read
        ran and the panel looked dead.
        """
        source = _source()
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertIn('xTaskCreate(files_scan_task, "files_scan"', create)
        self.assertRegex(create, r'xTaskCreate\(files_scan_task, "files_scan", \d+, state, 1,')

    def test_the_panel_needs_no_binding_before_it_opens(self):
        """The rows are buttons, so opening the panel costs nothing."""
        source = _source()
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertNotIn("bind_file_list", create)

    def test_the_rows_are_taken_when_the_read_is_done(self):
        source = _source()
        tick = _body(source, "static void upload_tick(esp_gsp_handle_t ui, void *ctx)\n{", "\nstatic void timer_tick")
        self.assertIn("files_scan_take(state)", tick)
        self.assertIn("refresh_file_list(state);", tick)

    def test_the_library_is_cleared_and_not_replaced(self):
        """The screen reads the library while the panel is open."""
        source = _source()
        reload_rows = _body(source, "static void reload_file_rows(struct app_hello *state)", "\nstatic void refresh_file_list")
        self.assertIn("recorder_library_clear(state->files_library);", reload_rows)
        self.assertNotIn("recorder_library_destroy", reload_rows)


class RecordingsPanelSceneTest(unittest.TestCase):
    """The recordings panel has to be rows the user can pick.

    The list widget renders the rows the application publishes, but it never
    reports a tapped row: neither the simulator nor the board delivered a
    row callback, and the list does not scroll either. The panel therefore
    uses buttons, the pattern the notes page already uses, and pages through
    the recordings.
    """

    def test_the_panel_has_no_list(self):
        """A list cannot be picked in this version of the framework."""
        scene = _scene()
        lists = [o for o in scene["objects"] if o.get("type") == "list"]
        self.assertEqual(lists, [], "the panel must not use a list widget")

    def test_every_row_is_a_button_with_its_own_callback(self):
        scene = _scene()
        rows = [o for o in scene["objects"] if str(o.get("name", "")).startswith("files_row_")
                and o.get("type") == "button"]
        self.assertEqual(len(rows), 5, "the panel shows five recordings at a time")
        for index, row in enumerate(rows):
            self.assertEqual(row.get("callback"), f"on_files_row_{index}")
            self.assertEqual(row.get("bind"), row.get("name"))
            self.assertEqual(row.get("bind_target"), "visible")

    def test_every_row_has_a_label_for_its_text(self):
        scene = _scene()
        names = {o.get("name") for o in scene["objects"]}
        for index in range(5):
            self.assertIn(f"files_row_text_{index}", names)

    def test_the_panel_pages_through_the_recordings(self):
        scene = _scene()
        for name in ("files_page_prev", "files_page_next"):
            entry = [o for o in scene["objects"] if o.get("name") == name]
            self.assertEqual(len(entry), 1, f"the panel has a {name} button")
            self.assertEqual(entry[0].get("callback"), f"on_{name}")

    def test_the_panel_shows_what_the_xiaozhi_upload_is_doing(self):
        """The panel says what the upload is doing.

        The serial log of the board drops lines, so the phase of the upload
        is on the screen: the user reads it and the screen is the evidence.
        """
        source = _source()
        self.assertIn("set_files_message(state, xiaozhi_note_phase_text(", source)

    def test_the_application_writes_the_rows_and_the_page(self):
        source = _source()
        self.assertIn("gsp_recorder_files_row_text_0_set_text", source)
        self.assertIn("gsp_recorder_files_page_set_text", source)
        self.assertIn("files_row_from_event", source)

    def test_the_first_row_is_the_newest_recording(self):
        """The listing of the tree is newest first.

        The panel shows the rows in the order of the listing, so the
        recording the user made last is the one on top. Turning the order
        around showed the oldest recordings first, and those were usually
        uploaded long ago.
        """
        source = _source()
        row_at = _body(source, "static const recorder_file_row_t *files_row_at(",
                       "\n/* Write the recordings of the current page")
        self.assertIn("recorder_library_at(state->files_library, offset)", row_at)

    def test_the_panel_is_shown_through_its_visibility_bind(self):
        """A container has no visibility property of its own. Without a bind
        the visibility call reports that it cannot find one and the panel
        stays closed."""
        scene = _scene()
        panel = _object(scene, "files_panel")
        self.assertEqual(panel.get("bind"), "files_panel")
        self.assertEqual(panel.get("bind_target"), "visible")
        source = _source()
        self.assertIn("GSP_RECORDER_BIND_FILES_PANEL", source)

    def test_the_panel_is_not_shown_through_a_component_call(self):
        source = _source()
        setter = _body(source, "static void set_files_visible(struct app_hello *state, bool visible)",
                       "\nstatic void sync_provision_chrome")
        self.assertIn("esp_gsp_set_visible(", setter)
        self.assertNotIn("esp_gsp_component_set_visible(", setter)


class SimulatorBuildTest(unittest.TestCase):
    """The simulator has to compile.

    The device-only code leaked out of the platform guard, so the simulator
    build broke and stayed broken. Every later change was verified on the
    device only, and the panel defects below reached the board unnoticed.
    """

    def test_the_device_only_scan_task_is_inside_the_device_guard(self):
        source = _source()
        self.assertTrue(_platform_guarded(source, "static void files_scan_task(void *arg)"),
                        "the scan task uses FreeRTOS and must not be built for the simulator")

    def test_the_upload_helpers_are_declared_outside_the_device_guard(self):
        source = _source()
        for declaration in ("static void request_upload(esp_gsp_handle_t ui, struct app_hello *state);",
                             "static void request_upload_sessions(esp_gsp_handle_t ui, struct app_hello *state,",
                             "static void start_upload(esp_gsp_handle_t ui, struct app_hello *state);"):
            self.assertFalse(_platform_guarded(source, declaration),
                             f"{declaration} is written for both builds and must be declared for both")

    def test_the_simulator_build_has_the_store_lock(self):
        cmake = (COMPONENT / "host" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("recorder_store_lock.c", cmake,
                      "the store lock has a host implementation and the simulator needs it")


class ChannelIndicatorTest(unittest.TestCase):
    """The screen has to say which channel the device is on.

    A recording went to one server while the user thought it went to
    another, because nothing on the screen named the channel. The capsule
    next to the wifi status carries it, and the button in the drawer names
    the channel it switches to.
    """

    def test_the_scene_has_a_channel_capsule(self):
        scene = _scene()
        pill = _object(scene, "channel_pill")
        text = _object(scene, "channel_text")
        self.assertEqual(text.get("bind"), "channel_text")
        self.assertLess(pill["x"], 290, "the capsule sits next to the wifi status")
        self.assertGreater(pill["x"] + pill["w"], 240, "the capsule does not cover the name")

    def test_the_application_names_the_channel(self):
        source = _source()
        self.assertIn("uplink_channel_label(", source)
        self.assertIn("gsp_recorder_channel_text_set_text(", source)

    def test_the_channel_is_painted_at_the_start(self):
        source = _source()
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertIn("paint_channel(state);", create)

    def test_the_settings_page_offers_both_channels(self):
        """The way back to the private channel has to be on the screen.

        The two channels are a choice, so the settings page carries a button
        for each of them. The way back is therefore always there, and it
        cannot be hidden behind a panel that is only open while something
        happens.

        Both buttons put the device on their channel. The switch is one
        function, because the two channels are a choice and they have to be
        chosen the same way: the screen has to be told, and the choice has
        to be kept.
        """
        scene = _settings_scene()
        private = _object(scene, "btn_channel_private")
        xiaozhi = _object(scene, "btn_channel_xiaozhi")
        self.assertEqual(private.get("callback"), "on_channel_private")
        self.assertEqual(xiaozhi.get("callback"), "on_channel_xiaozhi")
        source = _source()
        handler = _body(source, "if (gsp_settings_event_is_on_channel_private(event)) {",
                        "\nif (gsp_settings_event_is_on_channel_xiaozhi(event)) {")
        self.assertIn("switch_channel(state, uplink_channel_default());", handler)
        other = _body(source, "if (gsp_settings_event_is_on_channel_xiaozhi(event)) {",
                      "\nif (gsp_recorder_event_is_on_record(event)) {")
        self.assertIn("switch_channel(state, UPLINK_CHANNEL_XIAOZHI);", other)
        # The switch itself puts the device on the channel and tells the
        # screen about it.
        switch = _body(source, "static void switch_channel(struct app_hello *state, uplink_channel_t channel)\n{",
                       "\n/* Show which channel")
        self.assertIn("state->channel = uplink_channel_default();", switch)
        self.assertIn("state->channel = UPLINK_CHANNEL_XIAOZHI;", switch)
        self.assertIn("paint_channel(state);", switch)

    def test_the_settings_page_says_which_channel_is_on(self):
        scene = _settings_scene()
        status = _object(scene, "channel_status")
        self.assertEqual(status.get("bind"), "channel_status")
        source = _source()
        self.assertIn("gsp_settings_channel_status_set_text(", source)

    def test_the_channel_names_are_chinese(self):
        source = (COMPONENT / "uplink_channel.c").read_text(encoding="utf-8")
        self.assertIn("小智", source)
        self.assertIn("私有", source)


class PanelMessagePolicyTest(unittest.TestCase):
    """A message has to land on the screen the user is looking at.

    The recordings panel covers the main screen. The upload refused to start
    while a recording was running, and the refusal went to the main screen
    behind the panel, so the button looked like it had done nothing.
    """

    def _app(self) -> str:
        return (COMPONENT / "app_hello.c").read_text(encoding="utf-8")

    def test_the_upload_refusal_reaches_the_panel(self):
        source = self._app()
        upload = _body(source, "static void start_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{",
                       "\nstatic void paint_xiaozhi(")
        self.assertIn("set_upload_message(state, \"Cannot upload while recording\")", upload)
        self.assertNotIn('gsp_recorder_sub_text_set_text(ui, "Cannot upload while recording")', upload)

    def test_the_helper_writes_to_both_screens(self):
        source = self._app()
        helper = _body(source, "static void set_upload_message(struct app_hello *state, const char *text)\n{",
                       "\n}")
        self.assertIn("gsp_recorder_sub_text_set_text", helper)
        self.assertIn("if (state->files_visible)", helper)
        self.assertIn("set_files_message(state, text);", helper)


class RecorderWritePolicyTest(unittest.TestCase):
    """The screen must not wait for the recording to reach the flash.

    Writing the recording to the flash takes long enough that the button
    looked like it did nothing: the user pressed it again and nothing
    happened. The write happens on its own task and the screen says that it
    is saving.
    """

    def _app(self) -> str:
        return (COMPONENT / "app_hello.c").read_text(encoding="utf-8")

    def test_the_stop_happens_on_its_own_task(self):
        source = self._app()
        stop = _body(source, "static void toggle_recording(esp_gsp_handle_t ui, struct app_hello *state)\n{", "\nstatic void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{")
        # The task is made with the caps that put its stack in the external
        # memory: the encoder of the Opus codec needs a long stack for a
        # 60 ms frame and the internal memory is too small for it.
        self.assertIn("xTaskCreateWithCaps(recorder_stop_task", stop)
        self.assertIn("MALLOC_CAP_SPIRAM", stop)

    def test_the_write_reports_back(self):
        source = self._app()
        worker = _body(source, "static void recorder_stop_task(void *arg)", "\n/* Take the result")
        self.assertIn("recorder_mic_stop()", worker)
        self.assertIn("xQueueSend(state->recorder_results", worker)

    def test_the_result_is_taken_in_the_tick(self):
        source = self._app()
        tick = _body(source, "static void upload_tick(", "\nstatic void timer_tick")
        self.assertIn("take_recorder_result(state);", tick)

    def test_the_screen_says_that_it_is_saving(self):
        source = self._app()
        stop = _body(source, "static void toggle_recording(esp_gsp_handle_t ui, struct app_hello *state)\n{", "\nstatic void request_upload(esp_gsp_handle_t ui, struct app_hello *state)\n{")
        self.assertIn("Saving the recording", stop)


class SettingsPagePolicyTest(unittest.TestCase):

    def test_the_page_has_the_inbox_the_channel_and_the_binding(self):
        scene = _settings_scene()
        for name in ("btn_channel_private", "btn_channel_xiaozhi", "inbox_current",
                     "inbox_row_0", "inbox_row_1", "inbox_row_2", "xz_code", "xz_hint"):
            _object(scene, name)

    def test_the_page_is_the_third_scene(self):
        """The page is reached by swiping from the main page.

        The bundle has to name every page, in the order they are swiped.
        The helper of the shared tools takes one scene and puts it in the
        bundle as a single string, so a list of scenes is written as one
        string with semicolons: CMake reads that as a list, and the helper
        hands it to ESP-GSP, which takes a list of scenes.
        """
        bundle = (COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")
        call = bundle.split("mosaico_gsp_add_ui_bundle(", 1)[1].split(")", 1)[0]
        arguments = [part.strip().strip('"') for part in call.replace(";", " ").split()]
        self.assertEqual(arguments[1:], ["ui/main.json", "ui/notes.json", "ui/settings.json"])

    def test_a_row_of_the_inbox_is_a_choice(self):
        """A found inbox is never used without the user saying so."""
        scene = _settings_scene()
        row = _object(scene, "inbox_row_0")
        self.assertEqual(row.get("callback"), "on_inbox_row_0")
        self.assertEqual(row.get("bind_target"), "visible")
        source = _source()
        handler = _body(source, "} else if (inbox_row_from_event(event) >= 0) {",
                        "\n    } else if (wifi_key_from_event(event) >= 0) {")
        self.assertIn("inbox_discovery_confirm(", handler)

    def test_the_page_is_painted_while_it_is_open(self):
        """A candidate that appears has to show up without the user going
        back and forth."""
        source = _source()
        tick = _body(source, "static void upload_tick(", "\nstatic void timer_tick")
        self.assertIn("if (state->scene_id == 2) {", tick)
        self.assertIn("paint_settings(state);", tick)


class XiaozhiPanelEscapeTest(unittest.TestCase):
    """The Xiaozhi binding has to be reachable and escapable.

    The binding used to live in a panel that slid over the screen, and the
    way out of it was only there while the panel was open. The binding is
    now a section of the settings page, next to the channel choice, so the
    way out is always on the screen.
    """

    def test_the_binding_is_on_the_settings_page(self):
        source = _source()
        paint = _body(source, "static void paint_xiaozhi(struct app_hello *state)", "\nstatic void on_ui_event")
        self.assertIn("state->scene_id != 2", paint)
        self.assertIn("gsp_settings_xz_code_set_text(", paint)
        self.assertIn("gsp_settings_xz_hint_set_text(", paint)

    def test_the_binding_says_whether_the_channel_is_on(self):
        """The binding is only meaningful on the Xiaozhi channel, so the
        page says which channel is on."""
        source = _source()
        paint = _body(source, "static void paint_xiaozhi(struct app_hello *state)", "\nstatic void on_ui_event")
        self.assertIn("uplink_channel_runs_xiaozhi(state->channel)", paint)
        self.assertIn("小智通道未启用", paint)

    def test_the_way_out_is_always_on_the_page(self):
        """The user must be able to leave the Xiaozhi channel."""
        scene = _settings_scene()
        private = _object(scene, "btn_channel_private")
        self.assertIsNone(private.get("hidden"))
        self.assertNotEqual(private.get("bind_target"), "visible")

    def test_the_panel_says_what_is_happening(self):
        """The page must not be a blank sheet while the check-in runs."""
        source = _source()
        self.assertIn("xiaozhi_bind_view_waiting", source)
        # The waiting view carries a hint, so the page has text on it.
        channel = (COMPONENT / "uplink_channel.c").read_text(encoding="utf-8")
        waiting = _body(channel, "void xiaozhi_bind_view_waiting(", "\nint xiaozhi_bind_view_from_ota")
        self.assertIn("hint", waiting)


class ChannelPersistencePolicyTest(unittest.TestCase):
    """The device comes back on the channel it was on.

    A board that was put on the Xiaozhi channel used to go back to the
    private one after a restart, so the recording went to a server the user
    had not chosen for it any more.
    """

    def test_the_channel_is_read_at_the_start(self):
        source = _source()
        create = _body(source, "esp_gsp_err_t app_hello_create(", "\nvoid app_hello_destroy")
        self.assertIn("uplink_channel_load(&state->channel);", create)

    def test_the_channel_is_kept_when_it_changes(self):
        source = _source()
        self.assertIn("(void)uplink_channel_store(state->channel);", source)

    def test_the_channel_is_kept_in_nvs(self):
        channel = (COMPONENT / "uplink_channel.c").read_text(encoding="utf-8")
        self.assertIn("nvs_set_u8(", channel)
        self.assertIn("nvs_get_u8(", channel)
        self.assertIn("nvs_commit(", channel)

    def test_only_the_two_channels_are_kept(self):
        """A value that is not a channel is not a setting."""
        channel = (COMPONENT / "uplink_channel.c").read_text(encoding="utf-8")
        store = _body(channel, "int uplink_channel_store(", "\nstatic const char *activation_code")
        self.assertIn("UPLINK_CHANNEL_PRIVATE", store)
        self.assertIn("UPLINK_CHANNEL_XIAOZHI", store)


if __name__ == "__main__":
    unittest.main()
