// SPDX-License-Identifier: Apache-2.0

#include "app_hello.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"
#include "provision_screen.h"
#include "wifi_key_place.h"
#include "wifi_link.h"
#include "wifi_picker.h"
#include "wifi_sheet.h"
#include "meeting_notes.h"
#include "meeting_notes_sync.h"
#if !defined(ESP_PLATFORM)
/* The desktop build has no log of ESP-IDF, so the calls go to the
 * standard output. */
#include "host_log.h"
#endif
#if defined(ESP_PLATFORM)
#include "inbox_discovery.h"
#include "recorder_inbox.h"
#endif
#include "uplink_channel.h"
#include "xiaozhi_bind.h"
#include "xiaozhi_notes.h"
#include "xiaozhi_upload.h"
#include "recorder_library.h"
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "recorder_async.h"
#include "recorder_control.h"
#include "recorder_mic.h"
#include "recorder_store.h"
#include "recorder_uplink.h"
#endif

#if defined(ESP_PLATFORM)
typedef struct {
    int sent;
    char failure[40];
} upload_result_t;
#endif

/* How many recordings one upload can carry. The panel lets the user pick
 * several rows, and every one of them is uploaded on its own. */
#define APP_HELLO_UPLOAD_SESSIONS 16
/* How many rows the recordings panel holds. */
#define APP_HELLO_FILES_ROWS 128
/* How many ticks the screen waits for the Xiaozhi upload task to let go of
 * the device before it gives up on the rest of the queue. The tick runs
 * every 200 ms, so this is a little over five seconds. */
#define APP_HELLO_XIAOZHI_QUEUE_TRIES 25
/* The stack of the upload worker.
 *
 * The worker opens the HTTPS client, so it runs the TLS handshake and the
 * DNS lookup on its own stack. The notes sync needs 16 KB for the same
 * chain, and this worker also holds the slice list of the recording it
 * sends. A smaller stack overflows inside the handshake, and the check for
 * that is not enabled, so the task never comes back and the screen waits
 * for an upload that will never finish. */
#define APP_HELLO_UPLOAD_STACK 16384

/* How many inboxes the panel shows at a time. */
#define APP_HELLO_INBOX_ROWS 3

/* The task that writes the recording to the flash.
 *
 * The write is not long, but it is long enough that the screen must not
 * wait for it.
 *
 * The stack has to carry the whole write path, and that path is deep:
 * the recording is written into a directory, and the commit of that
 * directory compacts it, which erases a block of the flash, which makes
 * the wear levelling layer move a page, which reads the flash through the
 * SPI bus. Measured with the compiler's stack usage report, that chain
 * needs about four kilobytes.
 *
 * With a stack of four kilobytes the chain did not fit, and the hardware
 * stack guard ended the task in the middle of the commit: the board
 * rebooted and the recording was lost. The stack is now twice what the
 * chain needs, so a deeper path or a larger recording still fits.
 *
 * The stack comes out of the internal memory, and the task exists only
 * while the recording is written. */
#define APP_HELLO_RECORDER_STACK 40960
#define APP_HELLO_RECORDER_PRIO 4

/* The task that owns the work of the channel.
 *
 * The stack is in the internal memory, like the stacks of the other tasks
 * of the application. The task waits, asks for work, and starts and ends
 * it. Starting the work of a channel goes the deepest: the queue of the
 * module is made, and the task is made, and both of those reach into the
 * memory of the system.
 *
 * The stack was measured at eight hundred and eighty-four bytes of the
 * path, and it is four times that, so a deeper path still fits. The
 * measurement only means something once the deepest path has really run,
 * which is why the log carries the number: the board says how much of the
 * stack the work needs every time the channel changes.
 *
 * The stack costs three kilobytes of the internal memory of the board, and
 * that memory is small. It buys the release of the work of a channel
 * before the other channel starts, which is worth more than the three
 * kilobytes. */
#define APP_HELLO_CHANNEL_STACK 3072
#define APP_HELLO_CHANNEL_PRIO 3

/* How often the task of the channel looks at what it has to do.
 *
 * The wait is short, so a change of the channel is answered at once. The
 * task does nothing in between: the pull of the minutes keeps its own
 * interval, and the check-in of the cloud ends by itself. */
#define APP_HELLO_CHANNEL_POLL_MS 200

/* A request of the interface, waiting for the task of the screen.
 *
 * The actions of the interface are done by the task of the screen, because
 * the screen is not to be touched from two tasks at once. A caller on
 * another task puts the request in the queue and waits for its turn.
 *
 * The request carries the channel, because the switch of the channel is a
 * request like the others. */
typedef struct {
    app_hello_command_t command;
    app_hello_channel_t channel;
} app_hello_request_t;

struct app_hello {
    esp_gsp_handle_t ui;
    bool recording;
    uint32_t current_elapsed_s;
    uint32_t session_count;
    uint32_t pending_count;
    app_hello_state_t state;
    app_hello_error_t error;
    app_hello_observer_t observer;
    void *observer_ctx;
    void *timer;
    /* The private upload owns the main sub-text until the next user action.
     * Other writers on this screen must leave it alone, so the outcome of an
     * upload stays readable instead of being replaced by a phase label. */
    bool upload_status_shown;
#if defined(ESP_PLATFORM)
    void *upload_timer;
    QueueHandle_t upload_results;
    QueueHandle_t command_requests;
    QueueHandle_t recorder_results;
    QueueHandle_t provision_payloads;
    QueueHandle_t provision_finished;
    QueueHandle_t wifi_offers;
    QueueHandle_t wifi_results;
    /* The choice of the channel, waiting for the task that owns the work
     * of the channel. */
    QueueHandle_t channel_requests;
    recorder_async_t upload_job;
#endif
    /* The channel whose work is running.
     *
     * The choice of the user is on the screen at once, but the work of the
     * channel changes a moment later: the work of the channel that is left
     * has to end first. This says which channel is working, so a host can
     * see that the board really let go of the other channel. */
    volatile uint8_t work_channel;
    /* Whether the device still needs a check-in to the cloud.
     *
     * The address of the cloud and the token live in memory only, so a
     * device that was restarted has to ask the cloud again. The task of the
     * screen learns the address from the reply of the check-in and writes
     * that down here, so the task that owns the work of the channel knows
     * whether a check-in is needed. */
    volatile bool xiaozhi_needs_bind;
    esp_gsp_err_t last_error;
    provision_screen_t provision;
    recorder_library_t *files_library;
    recorder_files_t files;
    bool files_visible;
    /* The channel that the settings page last showed, so the line that
     * says which channel is on is written when it changes. */
    uint8_t settings_channel;
    /* The recording is being written to the flash. The screen does not wait
     * for it. */
    bool recorder_busy;
    bool settings_labels;
    /* The page of the recordings that is on the screen. The panel shows a
     * few recordings at a time and pages through the rest. */
    uint8_t files_page;
    /* The recordings that are on the device.
     *
     * Reading the recordings tree costs seconds, because every file of every
     * recording is opened on a flash that is slow. The read therefore runs on
     * its own task, and the screen takes the result when it is there. The
     * screen used to read the tree itself, which left the application
     * blocked for the better part of a minute after every start, and left the
     * panel unresponsive for the same time every time it was opened. */
    recorder_session_info_t files_scan[APP_HELLO_FILES_ROWS];
    size_t files_scan_count;
    uint32_t files_scan_saved;
    uint32_t files_scan_pending;
    volatile bool files_scan_wanted;
    volatile bool files_scan_ready;
    /* The recordings the user picked for the upload that is running. An empty
     * list means every recording that is not uploaded yet. */
    uint32_t upload_sessions[APP_HELLO_UPLOAD_SESSIONS];
    size_t upload_session_count;
    /* The position in that list, so the Xiaozhi upload can go on with the
     * next recording when one of them is done. */
    size_t upload_session_pos;
    /* The next picked recording waits for the upload task to let go of the
     * device before it starts. The wait is bounded, so a task that never
     * ends cannot hold the queue forever. */
    bool xiaozhi_queue_wait;
    uint8_t xiaozhi_queue_tries;
    wifi_picker_t wifi;
    wifi_sheet_t wifi_sheet;
    bool wifi_keys_open;
    bool wifi_dirty;
    bool wifi_link_on;
    bool wifi_link_known;
    bool wifi_link_drawer;
    bool wifi_link_show;
    char link_ssid[33];
    uint8_t wifi_keys_applied;
    uint8_t wifi_key_page_shown;
    meeting_notes_t notes;
    uint16_t scene_id;
    uint8_t notes_revision;
    uint8_t notes_painted;
    uint8_t notes_step;
    uint8_t notes_detail_shown;
    uplink_channel_t channel;
    bool xiaozhi_open;
    bool xiaozhi_bound;
    char xiaozhi_url[160];
    char xiaozhi_token[512];
    uint32_t xiaozhi_session;
    int xiaozhi_version;
    xiaozhi_note_phase_t xiaozhi_phase;
    int xiaozhi_sim_step;
    bool xiaozhi_sim_bound;
    xiaozhi_bind_view_t xiaozhi;
    uint8_t xiaozhi_revision;
    uint8_t xiaozhi_painted;
};

typedef struct {
    wifi_network_t networks[16];
    size_t count;
} wifi_offer_t;

typedef struct {
    bool join;
    char ssid[33];
    char password[64];
} wifi_result_t;

static void sync_provision_chrome(struct app_hello *state);
static void present_provision(struct app_hello *state, const char *payload);
static void refresh_wifi_list(struct app_hello *state);
static void paint_notes(struct app_hello *state);
static void paint_xiaozhi(struct app_hello *state);
static void bump_xiaozhi(struct app_hello *state);
static void switch_channel(struct app_hello *state, uplink_channel_t channel);
/* The recording and the upload are written once for both builds. Their
 * forward declarations used to sit inside the device-only block, so the
 * simulator could not compile the file at all. */
static void toggle_recording(esp_gsp_handle_t ui, struct app_hello *state);
static void request_upload(esp_gsp_handle_t ui, struct app_hello *state);
static void request_upload_sessions(esp_gsp_handle_t ui, struct app_hello *state, const uint32_t *sessions,
                                    size_t count);
static void start_upload(esp_gsp_handle_t ui, struct app_hello *state);
static void paint_channel(struct app_hello *state);
static void set_files_message(struct app_hello *state, const char *text);
static void take_notes(struct app_hello *state);
#if defined(ESP_PLATFORM)
static void paint_settings(struct app_hello *state);
static void recorder_stop_task(void *arg);
static void take_recorder_result(struct app_hello *state);
#endif

static void remember_error(struct app_hello *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

static void notify_observer(struct app_hello *state)
{
    if (!state->observer) {
        return;
    }
    app_hello_snapshot_t snapshot;
    if (app_hello_read(state, &snapshot) == ESP_GSP_OK) {
        state->observer(&snapshot, state->observer_ctx);
    }
}

static void render_inventory(esp_gsp_handle_t ui, struct app_hello *state)
{
    char files[48];
    char pending[32];
    snprintf(files, sizeof(files), "%u session%s saved", (unsigned)state->session_count,
             state->session_count == 1 ? "" : "s");
    snprintf(pending, sizeof(pending), "%u to sync", (unsigned)state->pending_count);
    remember_error(state, gsp_recorder_files_sub_set_text(ui, files));
    remember_error(state, gsp_recorder_pending_badge_set_text(ui, pending));
}

/* The status pill carries the short outcome token and the sub-text carries
 * the detail. Both are written together so the upload result is never only
 * half visible. */
static void set_files_message(struct app_hello *state, const char *text);
static void set_upload_message(struct app_hello *state, const char *text);

static void set_upload_status(struct app_hello *state, const char *pill, const char *text)
{
    state->upload_status_shown = true;
    remember_error(state, gsp_recorder_status_text_set_text(state->ui, pill));
    remember_error(state, gsp_recorder_sub_text_set_text(state->ui, text));
    /* The recordings panel covers the main screen, so the panel says it too.
     * Otherwise the outcome of an upload is on a screen that nobody sees
     * while the panel is open. */
    if (state->files_visible) {
        set_files_message(state, text);
    }
}

/* The recordings are shown as buttons, one page at a time.
 *
 * The list widget renders the rows the application publishes, but it never
 * reports a tapped row in this version of the framework: neither the
 * simulator nor the board delivered a row callback, and the list does not
 * scroll either. The panel therefore uses buttons, the same pattern the
 * notes page uses, and pages through the recordings. */
#define APP_HELLO_FILES_PAGE_ROWS 5

static esp_err_t (*const files_row_text_set[APP_HELLO_FILES_PAGE_ROWS])(esp_gsp_handle_t, const char *) = {
    gsp_recorder_files_row_text_0_set_text,
    gsp_recorder_files_row_text_1_set_text,
    gsp_recorder_files_row_text_2_set_text,
    gsp_recorder_files_row_text_3_set_text,
    gsp_recorder_files_row_text_4_set_text,
};

/* The visibility of every row button. A row without a recording is hidden,
 * so it cannot be tapped and does not leave an empty box on the screen. */
static const uint16_t files_row_bind[APP_HELLO_FILES_PAGE_ROWS] = {
    GSP_RECORDER_BIND_FILES_ROW_0,
    GSP_RECORDER_BIND_FILES_ROW_1,
    GSP_RECORDER_BIND_FILES_ROW_2,
    GSP_RECORDER_BIND_FILES_ROW_3,
    GSP_RECORDER_BIND_FILES_ROW_4,
};

/* The row the user tapped, or -1 when the event was another control. */
static int files_row_from_event(const esp_gsp_event_t *event)
{
    if (gsp_recorder_event_is_on_files_row_0(event)) {
        return 0;
    }
    if (gsp_recorder_event_is_on_files_row_1(event)) {
        return 1;
    }
    if (gsp_recorder_event_is_on_files_row_2(event)) {
        return 2;
    }
    if (gsp_recorder_event_is_on_files_row_3(event)) {
        return 3;
    }
    if (gsp_recorder_event_is_on_files_row_4(event)) {
        return 4;
    }
    return -1;
}

/* The inbox row the user tapped, or -1 when the event was another control. */
static int inbox_row_from_event(const esp_gsp_event_t *event)
{
    if (gsp_settings_event_is_on_inbox_row_0(event)) {
        return 0;
    }
    if (gsp_settings_event_is_on_inbox_row_1(event)) {
        return 1;
    }
    if (gsp_settings_event_is_on_inbox_row_2(event)) {
        return 2;
    }
    return -1;
}

#if !defined(ESP_PLATFORM)
/* The simulator has no recordings tree, so it shows a fixed list.
 *
 * The list mirrors the device: twelve saved recordings, each with the
 * timestamp the recorder writes, so the panel is checked with the same rows
 * and the same text the device shows. */
#define APP_HELLO_SIM_ROWS 12

static void reload_file_rows(struct app_hello *state)
{
    recorder_library_clear(state->files_library);
    recorder_files_init(&state->files);
    if (!state->files_library) {
        return;
    }
    for (uint32_t index = 0; index < APP_HELLO_SIM_ROWS; index++) {
        char stamp[16];
        uint32_t minutes = 9u * 60u + 40u - index * 7u;
        snprintf(stamp, sizeof(stamp), "20261114-%02u%02u", minutes / 60u, minutes % 60u);
        (void)recorder_library_add(state->files_library, index + 1u, 125000u - index * 9000u, false);
        (void)recorder_library_set_recorded_at(state->files_library, index + 1u, stamp);
    }
}
#endif

/* Read the recordings tree on its own task.
 *
 * Opening a file on this flash costs a moment and every recording opens a
 * few of them, so the read takes seconds. It used to run on the screen's
 * task, which left the application blocked for the better part of a minute
 * after every start and left the panel unresponsive for the same time every
 * time it was opened. */
#if defined(ESP_PLATFORM)
static void files_scan_task(void *arg)
{
    struct app_hello *state = arg;
    recorder_session_info_t *rows = heap_caps_malloc(sizeof(recorder_session_info_t) * APP_HELLO_FILES_ROWS,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows) {
        ESP_LOGE("app_hello", "the recordings scan has no memory for its rows");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        size_t count = 0;
        uint32_t saved = 0;
        uint32_t pending = 0;
        recorder_store_t *store;
        while (!state->files_scan_wanted) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        state->files_scan_wanted = false;
        store = recorder_store_open("/nand/recordings");
        if (store) {
            int64_t started_us = esp_timer_get_time();
            (void)recorder_store_scan(store, rows, APP_HELLO_FILES_ROWS, &count, &saved, &pending);
            ESP_LOGI("app_hello", "recordings scan: %u recording(s) in %d ms", (unsigned)count,
                     (int)((esp_timer_get_time() - started_us) / 1000));
            recorder_store_close(store);
        }
        memcpy(state->files_scan, rows, sizeof(rows[0]) * count);
        state->files_scan_count = count;
        state->files_scan_saved = saved;
        state->files_scan_pending = pending;
        state->files_scan_ready = true;
        /* The screen takes the result before the next read starts, so the
         * two never write over each other. */
        while (state->files_scan_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}
#endif

/* Ask for the recordings tree to be read. */
static void files_scan_request(struct app_hello *state)
{
#if defined(ESP_PLATFORM)
    if (!state->files_scan_wanted && !state->files_scan_ready) {
        state->files_scan_wanted = true;
    }
#else
    (void)state;
#endif
}

/* Take the recordings that were read, if they are there.
 *
 * Returns 1 when the rows changed, so the screen can show them. */
static int files_scan_take(struct app_hello *state)
{
#if defined(ESP_PLATFORM)
    if (!state->files_scan_ready) {
        return 0;
    }
    recorder_library_clear(state->files_library);
    for (size_t i = 0; i < state->files_scan_count; i++) {
        const recorder_session_info_t *row = &state->files_scan[i];
        (void)recorder_library_add(state->files_library, row->session_id, row->duration_ms, false);
        if (row->recorded_at[0] != '\0') {
            (void)recorder_library_set_recorded_at(state->files_library, row->session_id, row->recorded_at);
        }
    }
    state->session_count = state->files_scan_saved;
    state->pending_count = state->files_scan_pending;
    state->files_scan_ready = false;
    return 1;
#else
    (void)state;
    return 0;
#endif
}

/* How many pages the recordings take. */
static size_t files_page_count(const struct app_hello *state)
{
    size_t total = state->files_library ? recorder_library_count(state->files_library) : 0;
    if (total == 0) {
        return 1;
    }
    return (total + APP_HELLO_FILES_PAGE_ROWS - 1) / APP_HELLO_FILES_PAGE_ROWS;
}

/* The recording a row on the current page stands for.
 *
 * The listing of the tree is ordered newest first, so the first row is the
 * recording the user made last. Returns NULL when the row is past the last
 * recording. */
static const recorder_file_row_t *files_row_at(const struct app_hello *state, size_t row)
{
    size_t total = state->files_library ? recorder_library_count(state->files_library) : 0;
    size_t offset = (size_t)state->files_page * APP_HELLO_FILES_PAGE_ROWS + row;
    if (offset >= total) {
        return NULL;
    }
    return recorder_library_at(state->files_library, offset);
}

/* Write the recordings of the current page into the row buttons. */
static void refresh_file_list(struct app_hello *state)
{
    char page_text[16];
    if (!state->files_library) {
        return;
    }
    if ((size_t)state->files_page >= files_page_count(state)) {
        state->files_page = 0;
    }
    for (size_t row = 0; row < APP_HELLO_FILES_PAGE_ROWS; row++) {
        const recorder_file_row_t *file = files_row_at(state, row);
        char line[48];
        if (!file) {
            line[0] = '\0';
        } else {
            unsigned seconds = file->duration_ms / 1000u;
            if (file->recorded_at[0] != '\0') {
                snprintf(line, sizeof(line), "%s%s  %02u:%02u", file->selected ? "* " : "", file->recorded_at,
                         seconds / 60u, seconds % 60u);
            } else {
                snprintf(line, sizeof(line), "%s%" PRIu32 "  %02u:%02u", file->selected ? "* " : "",
                         file->session_id, seconds / 60u, seconds % 60u);
            }
        }
        remember_error(state, files_row_text_set[row](state->ui, line));
        /* A row without a recording must not answer a tap. */
        remember_error(state, esp_gsp_set_visible(state->ui, files_row_bind[row], file != NULL));
    }
    snprintf(page_text, sizeof(page_text), "%u/%u", (unsigned)state->files_page + 1u,
             (unsigned)files_page_count(state));
    remember_error(state, gsp_recorder_files_page_set_text(state->ui, page_text));
}

static void set_files_visible(struct app_hello *state, bool visible)
{
    state->files_visible = visible;
    /* The panel is shown through its visibility bind. A container has no
     * visibility property of its own, so the component call reports that it
     * cannot find one and the panel stays closed. */
    remember_error(state, esp_gsp_set_visible(state->ui, GSP_RECORDER_BIND_FILES_PANEL, visible));
    sync_provision_chrome(state);
}

#if defined(ESP_PLATFORM)
/* The inboxes that were found on the network.
 *
 * A found inbox is never used without the user saying so. The settings
 * page shows what was found, and a tap on a row makes that one the inbox
 * the device talks to. */
static esp_err_t (*const inbox_row_text_set[APP_HELLO_INBOX_ROWS])(esp_gsp_handle_t, const char *) = {
    gsp_settings_inbox_row_text_0_set_text,
    gsp_settings_inbox_row_text_1_set_text,
    gsp_settings_inbox_row_text_2_set_text,
};

static const uint16_t inbox_row_bind[APP_HELLO_INBOX_ROWS] = {
    GSP_SETTINGS_BIND_INBOX_ROW_0,
    GSP_SETTINGS_BIND_INBOX_ROW_1,
    GSP_SETTINGS_BIND_INBOX_ROW_2,
};

/* The address without its scheme, so the screen shows the host and the
 * port and not the whole url. */
static const char *origin_without_scheme(const char *origin)
{
    if (!origin) {
        return "";
    }
    if (strncmp(origin, "https://", 8) == 0) {
        return origin + 8;
    }
    if (strncmp(origin, "http://", 7) == 0) {
        return origin + 7;
    }
    return origin;
}

/* The settings page.
 *
 * It carries the three things that belong together: which channel the
 * device talks on, the inbox that the private channel talks to, and the
 * Xiaozhi binding. */
static void paint_settings(struct app_hello *state)
{
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char hint[64];
    size_t count;

    /* The scene carries only Latin fonts, so the Chinese on this page is
     * written here, once. */
    if (!state->settings_labels) {
        remember_error(state, gsp_settings_settings_title_set_text(state->ui, "设置"));
        remember_error(state, gsp_settings_channel_label_set_text(state->ui, "通道"));
        remember_error(state, gsp_settings_btn_channel_private_label_set_text(state->ui, "私有"));
        remember_error(state, gsp_settings_btn_channel_xiaozhi_label_set_text(state->ui, "小智"));
        remember_error(state, gsp_settings_inbox_label_set_text(state->ui, "收件箱"));
        remember_error(state, gsp_settings_xiaozhi_label_set_text(state->ui, "小智"));
        state->settings_labels = 1;
    }

    if (recorder_inbox_get(origin, sizeof(origin), NULL, 0) != 0) {
        origin[0] = '\0';
    }
    remember_error(state, gsp_settings_inbox_current_set_text(state->ui,
                                                                origin[0] ? origin_without_scheme(origin) : "还没有收件箱"));
    count = inbox_discovery_count();
    for (size_t row = 0; row < APP_HELLO_INBOX_ROWS; row++) {
        bool present = row < count;
        origin[0] = '\0';
        if (present) {
            (void)inbox_discovery_get(row, origin, sizeof(origin), NULL, 0);
        }
        remember_error(state, inbox_row_text_set[row](state->ui, origin[0] ? origin_without_scheme(origin) : ""));
        /* A row without an inbox must not answer a tap. */
        remember_error(state, esp_gsp_set_visible(state->ui, inbox_row_bind[row], present));
    }
    if (count == 0) {
        snprintf(hint, sizeof(hint), "没有找到收件箱");
    } else {
        snprintf(hint, sizeof(hint), "点一个用它");
    }
    remember_error(state, gsp_settings_inbox_hint_set_text(state->ui, hint));

    /* Which channel is on. The two channels are a choice, so the page says
     * which one it is, and the capsule on the main screen says the same.
     *
     * The line says what the channel is, because "私有" on its own does not
     * say that it is the inbox on the user's own machine. */
    if (state->settings_channel != (uint8_t)state->channel) {
        state->settings_channel = (uint8_t)state->channel;
        if (uplink_channel_runs_xiaozhi(state->channel)) {
            snprintf(hint, sizeof(hint), "当前通道：小智（小智云服务）");
        } else {
            snprintf(hint, sizeof(hint), "当前通道：私有（你自己的收件箱）");
        }
        remember_error(state, gsp_settings_channel_status_set_text(state->ui, hint));
    }
}
#endif

static void sync_provision_chrome(struct app_hello *state)
{
    bool setup = state->provision.deferred && !state->provision.visible && !state->files_visible;
    esp_gsp_err_t button = esp_gsp_set_visible(state->ui, GSP_RECORDER_BIND_BTN_SETUP, setup);
    esp_gsp_err_t pill = esp_gsp_set_visible(state->ui, GSP_RECORDER_BIND_STATUS_PILL, !setup);
    remember_error(state, button);
    remember_error(state, pill);
#if defined(ESP_PLATFORM)
    if (button != ESP_GSP_OK || pill != ESP_GSP_OK) {
        ESP_LOGW("app_hello", "provision visibility setup=%d pill=%d", (int)button, (int)pill);
    }
#endif
}

static esp_gsp_err_t (*const wifi_row_text[])(esp_gsp_handle_t, const char *) = {
    gsp_recorder_wifi_row_0_set_text, gsp_recorder_wifi_row_1_set_text, gsp_recorder_wifi_row_2_set_text,
    gsp_recorder_wifi_row_3_set_text, gsp_recorder_wifi_row_4_set_text, gsp_recorder_wifi_row_5_set_text,
    gsp_recorder_wifi_row_6_set_text, gsp_recorder_wifi_row_7_set_text,
};

static const gsp_component_key_t wifi_row_key[] = {
    GSP_RECORDER_OBJ_KEY_WIFI_ROW_0, GSP_RECORDER_OBJ_KEY_WIFI_ROW_1, GSP_RECORDER_OBJ_KEY_WIFI_ROW_2,
    GSP_RECORDER_OBJ_KEY_WIFI_ROW_3, GSP_RECORDER_OBJ_KEY_WIFI_ROW_4, GSP_RECORDER_OBJ_KEY_WIFI_ROW_5,
    GSP_RECORDER_OBJ_KEY_WIFI_ROW_6, GSP_RECORDER_OBJ_KEY_WIFI_ROW_7,
};

static const gsp_component_key_t wifi_key_key[] = {
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_0,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_1,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_2,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_3,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_4,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_5,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_6,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_7,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_8,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_9,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_10,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_11,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_12,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_13,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_14,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_15,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_16,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_17,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_18,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_19,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_20,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_21,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_22,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_23,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_24,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_25,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_26,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_27,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_28,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_29,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_30,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_31,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_32,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_33,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_34,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_35,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_36,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_37,
    GSP_RECORDER_OBJ_KEY_WIFI_KEY_38
};

static esp_gsp_err_t (*const wifi_key_text[])(esp_gsp_handle_t, const char *) = {
    gsp_recorder_wifi_key_0_set_text, gsp_recorder_wifi_key_1_set_text, gsp_recorder_wifi_key_2_set_text,
    gsp_recorder_wifi_key_3_set_text, gsp_recorder_wifi_key_4_set_text, gsp_recorder_wifi_key_5_set_text,
    gsp_recorder_wifi_key_6_set_text, gsp_recorder_wifi_key_7_set_text, gsp_recorder_wifi_key_8_set_text,
    gsp_recorder_wifi_key_9_set_text, gsp_recorder_wifi_key_10_set_text, gsp_recorder_wifi_key_11_set_text,
    gsp_recorder_wifi_key_12_set_text, gsp_recorder_wifi_key_13_set_text, gsp_recorder_wifi_key_14_set_text,
    gsp_recorder_wifi_key_15_set_text, gsp_recorder_wifi_key_16_set_text, gsp_recorder_wifi_key_17_set_text,
    gsp_recorder_wifi_key_18_set_text, gsp_recorder_wifi_key_19_set_text, gsp_recorder_wifi_key_20_set_text,
    gsp_recorder_wifi_key_21_set_text, gsp_recorder_wifi_key_22_set_text, gsp_recorder_wifi_key_23_set_text,
    gsp_recorder_wifi_key_24_set_text, gsp_recorder_wifi_key_25_set_text, gsp_recorder_wifi_key_26_set_text,
    gsp_recorder_wifi_key_27_set_text, gsp_recorder_wifi_key_28_set_text, gsp_recorder_wifi_key_29_set_text,
    gsp_recorder_wifi_key_30_set_text, gsp_recorder_wifi_key_31_set_text, gsp_recorder_wifi_key_32_set_text,
    gsp_recorder_wifi_key_33_set_text, gsp_recorder_wifi_key_34_set_text, gsp_recorder_wifi_key_35_set_text,
    gsp_recorder_wifi_key_36_set_text, gsp_recorder_wifi_key_37_set_text, gsp_recorder_wifi_key_38_set_text,
};

static int wifi_key_from_event(const esp_gsp_event_t *event)
{
    if (gsp_recorder_event_is_on_wifi_key_0(event)) return 0;
    if (gsp_recorder_event_is_on_wifi_key_1(event)) return 1;
    if (gsp_recorder_event_is_on_wifi_key_2(event)) return 2;
    if (gsp_recorder_event_is_on_wifi_key_3(event)) return 3;
    if (gsp_recorder_event_is_on_wifi_key_4(event)) return 4;
    if (gsp_recorder_event_is_on_wifi_key_5(event)) return 5;
    if (gsp_recorder_event_is_on_wifi_key_6(event)) return 6;
    if (gsp_recorder_event_is_on_wifi_key_7(event)) return 7;
    if (gsp_recorder_event_is_on_wifi_key_8(event)) return 8;
    if (gsp_recorder_event_is_on_wifi_key_9(event)) return 9;
    if (gsp_recorder_event_is_on_wifi_key_10(event)) return 10;
    if (gsp_recorder_event_is_on_wifi_key_11(event)) return 11;
    if (gsp_recorder_event_is_on_wifi_key_12(event)) return 12;
    if (gsp_recorder_event_is_on_wifi_key_13(event)) return 13;
    if (gsp_recorder_event_is_on_wifi_key_14(event)) return 14;
    if (gsp_recorder_event_is_on_wifi_key_15(event)) return 15;
    if (gsp_recorder_event_is_on_wifi_key_16(event)) return 16;
    if (gsp_recorder_event_is_on_wifi_key_17(event)) return 17;
    if (gsp_recorder_event_is_on_wifi_key_18(event)) return 18;
    if (gsp_recorder_event_is_on_wifi_key_19(event)) return 19;
    if (gsp_recorder_event_is_on_wifi_key_20(event)) return 20;
    if (gsp_recorder_event_is_on_wifi_key_21(event)) return 21;
    if (gsp_recorder_event_is_on_wifi_key_22(event)) return 22;
    if (gsp_recorder_event_is_on_wifi_key_23(event)) return 23;
    if (gsp_recorder_event_is_on_wifi_key_24(event)) return 24;
    if (gsp_recorder_event_is_on_wifi_key_25(event)) return 25;
    if (gsp_recorder_event_is_on_wifi_key_26(event)) return 26;
    if (gsp_recorder_event_is_on_wifi_key_27(event)) return 27;
    if (gsp_recorder_event_is_on_wifi_key_28(event)) return 28;
    if (gsp_recorder_event_is_on_wifi_key_29(event)) return 29;
    if (gsp_recorder_event_is_on_wifi_key_30(event)) return 30;
    if (gsp_recorder_event_is_on_wifi_key_31(event)) return 31;
    if (gsp_recorder_event_is_on_wifi_key_32(event)) return 32;
    if (gsp_recorder_event_is_on_wifi_key_33(event)) return 33;
    if (gsp_recorder_event_is_on_wifi_key_34(event)) return 34;
    if (gsp_recorder_event_is_on_wifi_key_35(event)) return 35;
    if (gsp_recorder_event_is_on_wifi_key_36(event)) return 36;
    if (gsp_recorder_event_is_on_wifi_key_37(event)) return 37;
    if (gsp_recorder_event_is_on_wifi_key_38(event)) return 38;
    return -1;
}

static void copy_connected_ssid(const struct app_hello *state, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
#if defined(ESP_PLATFORM)
    wifi_ap_record_t ap = {0};
    (void)state;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0] != '\0') {
        size_t length = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
        if (length >= cap) {
            length = cap - 1;
        }
        memcpy(out, ap.ssid, length);
        out[length] = '\0';
    }
#else
    if (state && state->link_ssid[0] != '\0') {
        snprintf(out, cap, "%s", state->link_ssid);
    }
#endif
}

static void refresh_wifi_list(struct app_hello *state)
{
    char status[80];
    char connected[33];
    char line[40];
    bool password = state->wifi.password_mode;
    size_t count = password ? 0 : state->wifi.count;
    copy_connected_ssid(state, connected, sizeof(connected));
    wifi_sheet_status(status, sizeof(status), password, state->wifi.password, count, connected);
    remember_error(state, gsp_recorder_wifi_status_set_text(state->ui, status));
    remember_error(state, gsp_recorder_btn_wifi_scan_set_text(state->ui, wifi_picker_shift_label(&state->wifi)));
    for (size_t i = 0; i < 8; i++) {
        bool show = i < count;
        line[0] = '\0';
        if (show) {
            snprintf(line, sizeof(line), "%s%s", state->wifi.networks[i].ssid,
                     state->wifi.networks[i].open ? "" : " *");
        }
        remember_error(state, wifi_row_text[i](state->ui, line));
        remember_error(state, esp_gsp_component_set_visible(state->ui, wifi_row_key[i], show));
    }
}

/* The simulator bridge drops a callback that updates every key at once.
 * Reveal or hide a few keys per timer tick. The device timer sends the
 * whole keyboard in one tick, outside the button callback. */
static void apply_wifi_keys(struct app_hello *state)
{
    bool want = wifi_sheet_keyboard_visible(state->wifi.password_mode, state->wifi_sheet.open);
    if (wifi_sheet_take_key_hide(&state->wifi_sheet)) {
        state->wifi_keys_applied = 0;
    }
    if (state->wifi_key_page_shown != (uint8_t)state->wifi.key_page) {
        state->wifi_key_page_shown = (uint8_t)state->wifi.key_page;
        state->wifi_keys_applied = 0;
    }
    size_t total = sizeof(wifi_key_key) / sizeof(wifi_key_key[0]);
    size_t budget = total;
    size_t step = 0;
    if (want != state->wifi_keys_open) {
        state->wifi_keys_open = want;
        state->wifi_keys_applied = 0;
    }
    if (state->wifi_keys_applied >= total) {
        return;
    }
#if !defined(ESP_PLATFORM)
    budget = 8;
#endif
    while (state->wifi_keys_applied < total && step < budget) {
        size_t index = state->wifi_keys_applied++;
        wifi_key_place_t place = wifi_key_place(index, want);
        remember_error(state, esp_gsp_component_set_position(state->ui, wifi_key_key[index], place.x, place.y));
        if (want) {
            char key = 0;
            char label[2] = {0};
            if (wifi_picker_key(&state->wifi, index, &key)) {
                label[0] = key;
                remember_error(state, wifi_key_text[index](state->ui, label));
            }
        }
        step++;
    }
}

static void mark_wifi_dirty(struct app_hello *state)
{
    state->wifi_dirty = true;
}

/* Button callbacks must not push a burst of UI updates through the simulator
 * bridge. The timer paints the list first, then the keyboard. */
static void paint_wifi(struct app_hello *state)
{
    if (state->wifi_dirty) {
        refresh_wifi_list(state);
        state->wifi_dirty = false;
#if !defined(ESP_PLATFORM)
        return;
#endif
    }
    apply_wifi_keys(state);
}

static bool station_has_address(void)
{
#if defined(ESP_PLATFORM)
    esp_netif_ip_info_t info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
#else
    return false;
#endif
}

static void paint_wifi_link(struct app_hello *state)
{
    bool connected = station_has_address();
    bool drawer = state->wifi_sheet.open;
    /* The recordings panel covers the screen, so the status of the main
     * screen must not show through it. */
    bool show = !drawer && !state->files_visible;
    esp_gsp_err_t err;
    if (state->wifi_link_known && connected == state->wifi_link_on && drawer == state->wifi_link_drawer &&
        show == state->wifi_link_show) {
        return;
    }
    err = gsp_recorder_wifi_on_set_text(state->ui, wifi_link_text(true));
    if (err != ESP_GSP_OK) {
        remember_error(state, err);
        return;
    }
    err = gsp_recorder_wifi_off_set_text(state->ui, wifi_link_text(false));
    if (err != ESP_GSP_OK) {
        remember_error(state, err);
        return;
    }
    err = esp_gsp_component_set_visible(state->ui, GSP_RECORDER_OBJ_KEY_WIFI_ON, show && connected);
    if (err != ESP_GSP_OK) {
        remember_error(state, err);
        return;
    }
    state->wifi_link_show = show;
    err = esp_gsp_component_set_visible(state->ui, GSP_RECORDER_OBJ_KEY_WIFI_OFF, show && !connected);
    if (err != ESP_GSP_OK) {
        remember_error(state, err);
        return;
    }
    state->wifi_link_known = true;
    state->wifi_link_on = connected;
    state->wifi_link_drawer = drawer;
}

static void queue_wifi_result(struct app_hello *state, bool join)
{
#if defined(ESP_PLATFORM)
    wifi_result_t result = {.join = join};
    if (join && wifi_picker_copy_join(&state->wifi, result.ssid, sizeof(result.ssid), result.password,
                                      sizeof(result.password)) != 0) {
        return;
    }
    if (join) {
        (void)esp_gsp_drawer_close(state->ui, GSP_RECORDER_OBJ_KEY_WIFI_PANEL, true);
    }
    (void)xQueueSend(state->wifi_results, &result, 0);
#else
    (void)state;
    (void)join;
#endif
}

static void present_provision(struct app_hello *state, const char *payload)
{
    if (!provision_screen_offer(&state->provision, payload)) {
        return;
    }
    sync_provision_chrome(state);
}

static void reopen_provision(struct app_hello *state)
{
    if (!provision_screen_reopen(&state->provision)) {
        return;
    }
    sync_provision_chrome(state);
}

static void finish_provision(struct app_hello *state)
{
    provision_screen_complete(&state->provision);
    sync_provision_chrome(state);
    if (!state->recording && state->state == APP_HELLO_STATE_IDLE && !state->upload_status_shown) {
        remember_error(state, gsp_recorder_sub_text_set_text(state->ui, "Tap Record to start"));
    }
}

#if defined(ESP_PLATFORM)
static void refresh_inventory(struct app_hello *state)
{
    /* The counts come from the same read as the list, and that read is slow,
     * so it is asked for and the screen takes the result when it is there. */
    files_scan_request(state);
}

static void upload_worker(void *ctx)
{
    struct app_hello *state = ctx;
    upload_result_t result = {.sent = -1};
    char device_id[16];
    /* The selection is copied, so the panel can change while the upload
     * runs without changing what this upload sends. */
    uint32_t sessions[APP_HELLO_UPLOAD_SESSIONS];
    size_t session_count = state->upload_session_count;
    recorder_uplink_device_id(device_id, sizeof(device_id));
    if (session_count > APP_HELLO_UPLOAD_SESSIONS) {
        session_count = APP_HELLO_UPLOAD_SESSIONS;
    }
    memcpy(sessions, state->upload_sessions, sizeof(sessions[0]) * session_count);
    ESP_LOGI("app_hello", "upload worker: %u recording(s) picked", (unsigned)session_count);
    result.sent = recorder_uplink_send_pending("/nand/recordings", device_id,
                                                session_count > 0 ? sessions : NULL, session_count);
    ESP_LOGI("app_hello", "upload worker: done sent=%d", (int)result.sent);
    if (result.sent < 0) {
        snprintf(result.failure, sizeof(result.failure), "%s", recorder_uplink_failure());
    } else if (result.sent == 0) {
        const char *note = recorder_uplink_note();
        if (note) {
            snprintf(result.failure, sizeof(result.failure), "%s", note);
        }
    }
    (void)xQueueSend(state->upload_results, &result, portMAX_DELAY);
    vTaskDelete(NULL);
}

/* Mark the minutes as changed, so the page is painted again.
 *
 * The page is painted in steps, one step per tick, and the steps belong to
 * one version of the minutes. A new version starts the page from the top:
 * a cycle that was cut short by leaving the page would otherwise resume in
 * the middle, and the list of the minutes would never be painted again.
 * That is what made the page come back empty after a card was closed.
 *
 * A card that is open shows its own pages, so the list is skipped and the
 * page starts at the conclusion. Otherwise the page starts at the top.
 */
static void bump_notes(struct app_hello *state)
{
    state->notes_revision++;
    state->notes_step = meeting_notes_is_open(&state->notes) ? 6 : 0;
}

static void upload_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    upload_result_t completed;
    app_hello_request_t request;
    int sent;
    if (!state) {
        return;
    }
    while (xQueueReceive(state->command_requests, &request, 0) == pdTRUE) {
        if (request.command == APP_HELLO_COMMAND_TOGGLE_RECORD) {
            toggle_recording(ui, state);
        } else if (request.command == APP_HELLO_COMMAND_UPLOAD) {
            request_upload(ui, state);
        } else if (request.command == APP_HELLO_COMMAND_CHANNEL) {
            switch_channel(state, request.channel == APP_HELLO_CHANNEL_XIAOZHI ? UPLINK_CHANNEL_XIAOZHI
                                                                             : UPLINK_CHANNEL_PRIVATE);
        }
    }
    /* The recordings tree was read on its own task. Take the result when it
     * is there, so the screen shows the recordings without waiting for the
     * read. */
    if (files_scan_take(state)) {
        if (state->files_visible) {
            refresh_file_list(state);
        }
        if (state->scene_id == 0) {
            render_inventory(ui, state);
        }
    }
    {
        char *payload = NULL;
        while (xQueueReceive(state->provision_payloads, &payload, 0) == pdTRUE) {
            present_provision(state, payload);
            free(payload);
        }
        uint8_t finished = 0;
        if (xQueueReceive(state->provision_finished, &finished, 0) == pdTRUE) {
            finish_provision(state);
        }
    }
    {
        wifi_offer_t offer;
        if (xQueueReceive(state->wifi_offers, &offer, 0) == pdTRUE) {
            wifi_picker_set_scan(&state->wifi, offer.networks, offer.count);
            mark_wifi_dirty(state);
        }
        if (state->scene_id == 0) {
            bool open = false;
            if (esp_gsp_drawer_is_open(ui, GSP_RECORDER_OBJ_KEY_WIFI_PANEL, &open) == ESP_GSP_OK) {
                wifi_sheet_note(&state->wifi_sheet, open ? 1 : 0, false, true);
                if (wifi_sheet_take_closed(&state->wifi_sheet)) {
                    wifi_picker_show_networks(&state->wifi);
                    mark_wifi_dirty(state);
                }
                if (wifi_sheet_take_scan(&state->wifi_sheet)) {
                    remember_error(state, gsp_recorder_wifi_status_set_text(ui, "Searching"));
                    queue_wifi_result(state, false);
                }
            }
            paint_wifi(state);
            paint_wifi_link(state);
        }
#if defined(ESP_PLATFORM)
        {
            char *body = xiaozhi_bind_take_body();
            int status = 0;
            if (body) {
                (void)xiaozhi_bind_websocket(body, state->xiaozhi_url, sizeof(state->xiaozhi_url), state->xiaozhi_token,
                                             sizeof(state->xiaozhi_token), &state->xiaozhi_version);
                /* The address of the cloud and the token live in memory
                 * only. Now they are known, so the task of the channel does
                 * not have to ask the cloud again. */
                if (state->xiaozhi_url[0] != '\0') {
                    state->xiaozhi_needs_bind = false;
                }
                /* A device that is already bound on the server answers the
                 * check-in without an activation section. The activate poll
                 * never runs for it, so the bound state has to come from the
                 * check-in reply itself. */
                if (xiaozhi_bind_already_bound(body) == 1) {
                    state->xiaozhi_bound = true;
                    state->xiaozhi.show_code = false;
                    snprintf(state->xiaozhi.hint, sizeof(state->xiaozhi.hint), "%s", "已绑定");
                    /* Close the panel like the first-time bind does, so the
                     * user lands on the main screen with the Xiaozhi channel
                     * still selected and can press Upload. */
                    state->xiaozhi_open = false;
                    if (!state->upload_status_shown) {
                        remember_error(state, gsp_recorder_sub_text_set_text(ui, "已绑定"));
                    }
                    bump_xiaozhi(state);
                }
                if (state->xiaozhi_open && xiaozhi_bind_view_from_ota(body, &state->xiaozhi) == 0) {
                    bump_xiaozhi(state);
                }
                free(body);
            }
            if (xiaozhi_bind_take_status(&status) && xiaozhi_bind_from_http(status) == 1) {
                state->xiaozhi_bound = true;
                state->xiaozhi.show_code = false;
                snprintf(state->xiaozhi.hint, sizeof(state->xiaozhi.hint), "%s", "已绑定");
                state->xiaozhi_open = false;
                bump_xiaozhi(state);
            }
        }
#endif
#if defined(ESP_PLATFORM)
        /* Look for an inbox that announces itself on the network. mDNS needs
         * an interface, so this starts once the station has an address. A
         * found inbox is not used until the user says so. */
        if (station_has_address()) {
            inbox_discovery_start();

        }
#endif
#if defined(ESP_PLATFORM)
        take_notes(state);
#endif
        if (state->scene_id == 1) {
            paint_notes(state);
        }
#if defined(ESP_PLATFORM)
        take_recorder_result(state);
#endif
#if defined(ESP_PLATFORM)
        if (state->scene_id == 2) {
            /* The settings page carries the inbox, so it is refreshed while
             * the page is open: a candidate that appears has to show up
             * without the user going back and forth. */
            paint_settings(state);
            paint_xiaozhi(state);
        }
#endif
    }
#if defined(ESP_PLATFORM)
    {
        xiaozhi_upload_result_t result;
        int phase = xiaozhi_upload_phase();
        /* The private upload owns the main sub-text until the next user
         * action, so a Xiaozhi phase label must not replace it. */
        if (phase != XIAOZHI_NOTE_IDLE && phase != (int)state->xiaozhi_phase) {
            state->xiaozhi_phase = (xiaozhi_note_phase_t)phase;
            if (!state->upload_status_shown) {
                remember_error(state, gsp_recorder_sub_text_set_text(ui, xiaozhi_note_phase_text(state->xiaozhi_phase)));
            }
            if (state->files_visible) {
                set_files_message(state, xiaozhi_note_phase_text(state->xiaozhi_phase));
            }
        }
        while (xiaozhi_upload_take(&result)) {
            state->xiaozhi_phase = result.phase;
            if (result.state == XIAOZHI_UPLOAD_SUCCEEDED) {
                if (meeting_notes_merge_document(&state->notes, result.card) == 0) {
                    meeting_notes_save(&state->notes, MEETING_NOTES_PATH);
                    bump_notes(state);
                }
                if (!state->upload_status_shown) {
                    remember_error(state, gsp_recorder_sub_text_set_text(ui, xiaozhi_note_phase_text(result.phase)));
                }
                if (state->files_visible) {
                    set_files_message(state, xiaozhi_note_phase_text(result.phase));
                }
            } else {
                state->error = APP_HELLO_ERROR_NETWORK;
                /* The reason is on the screen, not only in the log.
                 *
                 * The log of the board drops lines, so a failure that is
                 * only in the log is a failure the user cannot report and
                 * the developer cannot see. The reason says what to do:
                 * the server did not answer, or it answered without the
                 * fields of a card. What the server said is shown too,
                 * because that is what tells the two apart. */
                if (!state->upload_status_shown) {
                    remember_error(state, gsp_recorder_sub_text_set_text(ui, xiaozhi_upload_failure_text(result.failure, result.answer)));
                }
                if (state->files_visible) {
                    set_files_message(state, xiaozhi_note_phase_text(XIAOZHI_NOTE_FAILED));
                }
            }
            notify_observer(state);
            /* The Xiaozhi upload carries one meeting per connection, so the
             * next picked recording goes out when this one is done. */
            if (state->upload_session_count > 0 &&
                state->upload_session_pos + 1 < state->upload_session_count) {
                state->upload_session_pos++;
                state->xiaozhi_queue_wait = true;
            } else {
                state->upload_session_count = 0;
                state->upload_session_pos = 0;
            }
        }
        /* The upload task lets go of the device a moment after it hands over
         * its result, so the next recording waits for that. The wait is
         * bounded: a task that never ends cannot hold the queue forever. */
        if (state->xiaozhi_queue_wait) {
            if (state->xiaozhi_queue_tries >= APP_HELLO_XIAOZHI_QUEUE_TRIES) {
                state->xiaozhi_queue_wait = false;
                state->xiaozhi_queue_tries = 0;
                state->upload_session_count = 0;
                state->upload_session_pos = 0;
#if defined(ESP_PLATFORM)
                ESP_LOGE("app_hello", "the Xiaozhi upload did not let go of the device; the queue is dropped");
#endif
            } else {
                state->xiaozhi_queue_tries++;
                if (!xiaozhi_upload_busy()) {
                    state->xiaozhi_queue_wait = false;
                    state->xiaozhi_queue_tries = 0;
                    start_upload(ui, state);
                }
            }
        }
    }
#endif
    if (xQueueReceive(state->upload_results, &completed, 0) != pdTRUE) {
        return;
    }
    recorder_async_complete(&state->upload_job, completed.sent);
    if (!recorder_async_take_result(&state->upload_job, &sent)) {
        return;
    }
    refresh_inventory(state);
    if (sent < 0) {
        state->state = APP_HELLO_STATE_ERROR;
        state->error = APP_HELLO_ERROR_NETWORK;
    } else {
        state->state = APP_HELLO_STATE_UPLOAD_OK;
        state->error = APP_HELLO_ERROR_NONE;
    }
    if (state->scene_id == 0) {
        render_inventory(ui, state);
        if (sent < 0) {
            set_upload_status(state, "ERR",
                              completed.failure[0] ? completed.failure : "Upload failed");
        } else if (sent == 0) {
            set_upload_status(state, "OK", "All sessions up to date");
        } else {
            set_upload_status(state, "OK", "Uploaded");
        }
    }
    if (state->files_visible) {
        /* The panel said "Uploading" when the user pressed the button. It
         * has to say what happened, or the panel looks stuck. */
        char outcome[40];
        if (sent < 0) {
            snprintf(outcome, sizeof(outcome), "%s",
                     completed.failure[0] ? completed.failure : "Upload failed");
        } else if (sent == 0) {
            snprintf(outcome, sizeof(outcome), "%s",
                     completed.failure[0] ? completed.failure : "Nothing to upload");
        } else {
            snprintf(outcome, sizeof(outcome), "Uploaded %d", sent);
        }
        remember_error(state, gsp_recorder_files_confirm_set_text(ui, outcome));
    }
    notify_observer(state);
}
#endif

static void timer_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    char time_str[16];
    if (!state || !state->recording) {
        return;
    }
#if defined(ESP_PLATFORM)
    if (recorder_mic_fault()) {
        toggle_recording(ui, state);
        return;
    }
    {
        uint32_t elapsed_ms = recorder_mic_elapsed_ms();
        state->current_elapsed_s = elapsed_ms / 1000u;
        if (recorder_clock_format(elapsed_ms, time_str, sizeof(time_str)) != 0) {
            return;
        }
    }
#else
    state->current_elapsed_s++;
    uint32_t mins = state->current_elapsed_s / 60;
    uint32_t secs = state->current_elapsed_s % 60;
    snprintf(time_str, sizeof(time_str), "%02u:%02u", (unsigned)mins, (unsigned)secs);
#endif
    if (state->scene_id == 0) {
        remember_error(state, gsp_recorder_timer_set_text(ui, time_str));
    }
}

static void toggle_recording(esp_gsp_handle_t ui, struct app_hello *state)
{
    /* Recording is a user action, so it takes the main screen back from the
     * upload result. */
    state->upload_status_shown = false;
    if (!state->recording) {
#if defined(ESP_PLATFORM)
        if (recorder_mic_start() != 0) {
            state->state = APP_HELLO_STATE_ERROR;
            state->error = APP_HELLO_ERROR_MIC;
            remember_error(state, gsp_recorder_sub_text_set_text(ui, recorder_mic_failure()));
            notify_observer(state);
            return;
        }
#endif
        state->recording = true;
        state->state = APP_HELLO_STATE_RECORDING;
        state->error = APP_HELLO_ERROR_NONE;
        state->current_elapsed_s = 0;
        remember_error(state, gsp_recorder_status_text_set_text(ui, "REC"));
        remember_error(state, gsp_recorder_btn_record_label_set_text(ui, "Stop"));
        remember_error(state, gsp_recorder_timer_set_text(ui, "00:00"));

        char sub[64];
        snprintf(sub, sizeof(sub), "Recording session #%u...", (unsigned)(state->session_count + 1));
        remember_error(state, gsp_recorder_sub_text_set_text(ui, sub));

        state->timer = esp_gsp_timer_create(ui, 1000, timer_tick, state);
        if (!state->timer) {
            remember_error(state, ESP_GSP_ERR_NO_MEM);
        }
        notify_observer(state);
        return;
    }
#if defined(ESP_PLATFORM)
    state->current_elapsed_s = recorder_mic_elapsed_ms() / 1000u;
    /* Writing the recording to the flash takes a moment, and the screen must
     * not wait for it: the button has to answer at once, or the user presses
     * it again and nothing happens.
     *
     * The screen says that it is saving, and the recording lands a moment
     * later. The audio is already in memory, so nothing is lost by stopping
     * a moment later. */
    if (!state->recorder_busy) {
        state->recorder_busy = true;
        remember_error(state, gsp_recorder_status_text_set_text(ui, "SAVE"));
        remember_error(state, gsp_recorder_sub_text_set_text(ui, "Saving the recording"));
        /* The stack of the task is in the external memory: the last frame
         * of the recording is encoded here, and the encoder of the Opus
         * codec needs a long stack for a 60 ms frame. The internal memory
         * of the board is too small to give it one there. */
        if (xTaskCreateWithCaps(recorder_stop_task, "rec_stop", APP_HELLO_RECORDER_STACK, state,
                                APP_HELLO_RECORDER_PRIO, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            /* Without the task the recording has to be written here, and the
             * screen waits for it after all. */
            state->recorder_busy = false;
            (void)recorder_mic_stop();
        }
    }
#endif
    state->recording = false;
    state->state = APP_HELLO_STATE_IDLE;
    state->error = APP_HELLO_ERROR_NONE;
    if (state->timer) {
        remember_error(state, esp_gsp_timer_delete(ui, state->timer));
        state->timer = NULL;
    }
    state->session_count++;
    state->pending_count++;

    remember_error(state, gsp_recorder_status_text_set_text(ui, "IDLE"));
    remember_error(state, gsp_recorder_btn_record_label_set_text(ui, "Record"));
    paint_channel(state);

    uint32_t mins = state->current_elapsed_s / 60;
    uint32_t secs = state->current_elapsed_s % 60;
    char sub[64];
    snprintf(sub, sizeof(sub), "Saved session #%u (%02u:%02u)",
             (unsigned)state->session_count, (unsigned)mins, (unsigned)secs);
    remember_error(state, gsp_recorder_sub_text_set_text(ui, sub));
    render_inventory(ui, state);
    notify_observer(state);
}

static void request_upload(esp_gsp_handle_t ui, struct app_hello *state)
{
    /* The main screen uploads everything that is not uploaded yet, so the
     * selection is cleared. */
    state->upload_session_count = 0;
    state->upload_session_pos = 0;
    state->xiaozhi_queue_wait = false;
    state->xiaozhi_queue_tries = 0;
    start_upload(ui, state);
}

static void request_upload_sessions(esp_gsp_handle_t ui, struct app_hello *state, const uint32_t *sessions,
                                    size_t count)
{
    /* The panel uploads the recordings the user picked. */
    state->upload_session_count = 0;
    state->upload_session_pos = 0;
    state->xiaozhi_queue_wait = false;
    state->xiaozhi_queue_tries = 0;
    if (sessions && count > 0) {
        size_t kept = count > APP_HELLO_UPLOAD_SESSIONS ? APP_HELLO_UPLOAD_SESSIONS : count;
        memcpy(state->upload_sessions, sessions, sizeof(state->upload_sessions[0]) * kept);
        state->upload_session_count = kept;
    }
    start_upload(ui, state);
}
/* Start the upload of the recordings in the selection.
 *
 * An empty selection means every recording that is not uploaded yet. The
 * panel puts the recordings the user picked into the selection, so only
 * those are sent. */
static void start_upload(esp_gsp_handle_t ui, struct app_hello *state)
{
    /* Pressing Upload is a user action, so it takes the main screen back
     * from the previous upload result. The private path sets it again. */
    state->upload_status_shown = false;
    if (uplink_channel_runs_xiaozhi(state->channel)) {
        if (state->recording) {
            state->error = APP_HELLO_ERROR_BUSY;
            set_upload_message(state, "Cannot upload while recording");
            notify_observer(state);
            return;
        }
        state->error = APP_HELLO_ERROR_NONE;
        if (!state->xiaozhi_bound || state->xiaozhi_url[0] == '\0' || state->xiaozhi_token[0] == '\0') {
            /* The device is not bound yet, so there is no address to send
             * the recording to. The check-in runs again, so the user only
             * has to press Upload once the device is bound. */
            ESP_LOGW("app_hello", "the Xiaozhi upload needs a bound device; the check-in is started");
#if defined(ESP_PLATFORM)
            xiaozhi_bind_start();
#endif
            set_files_message(state, "Binding the Xiaozhi channel");
            set_upload_message(state, "请先绑定");
            notify_observer(state);
            return;
        }
#if defined(ESP_PLATFORM)
        {
            uint32_t session = 0;
            if (state->upload_session_count > 0) {
                session = state->upload_sessions[state->upload_session_pos];
            } else {
                session = xiaozhi_upload_next_session();
            }
            if (session == 0) {
                set_upload_message(state, "All sessions up to date");
                notify_observer(state);
                return;
            }
            if (xiaozhi_upload_start(state->xiaozhi_url, state->xiaozhi_token, state->xiaozhi_version,
                                     session) != 0) {
                set_upload_message(state, "Upload already running");
                notify_observer(state);
                return;
            }
            state->xiaozhi_session = session;
        }
#endif
        state->xiaozhi_phase = XIAOZHI_NOTE_PACKING;
#if !defined(ESP_PLATFORM)
        state->xiaozhi_sim_step = 1;
#endif
        remember_error(state, gsp_recorder_sub_text_set_text(ui, xiaozhi_note_phase_text(state->xiaozhi_phase)));
        notify_observer(state);
        return;
    }
    if (!uplink_channel_runs_private(state->channel)) {
        set_upload_message(state, "Xiaozhi channel");
        return;
    }
    if (state->recording) {
        state->error = APP_HELLO_ERROR_BUSY;
        set_upload_message(state, "Cannot upload while recording");
        notify_observer(state);
        return;
    }
#if defined(ESP_PLATFORM)
    if (recorder_async_request(&state->upload_job) != 0) {
        state->state = APP_HELLO_STATE_UPLOADING;
        state->error = APP_HELLO_ERROR_BUSY;
        set_upload_status(state, "ERR", "Upload already running");
        notify_observer(state);
    } else if (xTaskCreate(upload_worker, "rec_upload", APP_HELLO_UPLOAD_STACK, state, 5, NULL) != pdPASS) {
        int ignored;
        recorder_async_complete(&state->upload_job, -1);
        (void)recorder_async_take_result(&state->upload_job, &ignored);
        state->state = APP_HELLO_STATE_ERROR;
        state->error = APP_HELLO_ERROR_BUSY;
        set_upload_status(state, "ERR", "Cannot start upload");
        notify_observer(state);
    } else {
        state->state = APP_HELLO_STATE_UPLOADING;
        state->error = APP_HELLO_ERROR_NONE;
        /* The pill shows that the upload started, so the user can tell an
         * upload in flight from one that never began. */
        set_upload_status(state, "SYNC", "Uploading");
        notify_observer(state);
    }
#else
    state->state = APP_HELLO_STATE_UPLOAD_OK;
    state->error = APP_HELLO_ERROR_NONE;
    if (state->pending_count > 0) {
        set_upload_status(state, "OK", "Ready to sync with Mac inbox");
    } else {
        set_upload_status(state, "OK", "All sessions up to date");
    }
    notify_observer(state);
#endif
}

#if defined(ESP_PLATFORM)
/* Write the recording to the flash, away from the screen.
 *
 * The write is the slow part of stopping a recording, and the screen must
 * stay alive while it happens.
 *
 * The log says how much of the stack the write needed. The write path is
 * deep, and it grew once already: a stack that was too small ended the
 * task in the middle of the commit, the board rebooted and the recording
 * was lost. The number in the log is the way to see that coming, instead
 * of finding it out from a missing file.
 *
 * The high water mark of ESP-IDF is in bytes, where the FreeRTOS
 * documentation says words. */
static void recorder_stop_task(void *arg)
{
    struct app_hello *state = arg;
    int failed = recorder_mic_stop() != 0;
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("app_hello", "the write used %u bytes of the %u byte stack",
             (unsigned)APP_HELLO_RECORDER_STACK - (unsigned)free_bytes,
             (unsigned)APP_HELLO_RECORDER_STACK);
    if (state->recorder_results) {
        (void)xQueueSend(state->recorder_results, &failed, 0);
    }
    vTaskDelete(NULL);
}

/* Take the result of the write and say what happened. */
static void take_recorder_result(struct app_hello *state)
{
    int failed = 0;
    if (!state->recorder_results || xQueueReceive(state->recorder_results, &failed, 0) != pdTRUE) {
        return;
    }
    state->recorder_busy = false;
    if (failed) {
        state->error = APP_HELLO_ERROR_STORAGE;
        remember_error(state, gsp_recorder_sub_text_set_text(state->ui, "Could not save recording"));
    } else {
        remember_error(state, gsp_recorder_sub_text_set_text(state->ui, "Recording saved"));
    }
    notify_observer(state);
}
#endif

static void show_note(struct app_hello *state, gsp_component_key_t key, bool show)
{
    remember_error(state, esp_gsp_component_set_visible(state->ui, key, show));
}

#if defined(ESP_PLATFORM)
/* Take the minutes the server sent and keep them.
 *
 * The minutes arrive at any time, not only while the minutes page is open.
 * They used to be taken by the page, so the document the server sent every
 * minute sat in the queue until the page was opened, and the next document
 * was thrown away with "the notes queue was full". The copy on the flash is
 * written only when the minutes really changed, because the server sends
 * the same minutes over and over. */
static void take_notes(struct app_hello *state)
{
    char *delivered = meeting_notes_sync_take();
    unsigned before;
    unsigned after;
    if (!delivered) {
        return;
    }
    before = meeting_notes_fingerprint(&state->notes);
    if (meeting_notes_merge_document(&state->notes, delivered) == 0) {
        after = meeting_notes_fingerprint(&state->notes);
        if (after != before) {
            meeting_notes_save(&state->notes, MEETING_NOTES_PATH);
            bump_notes(state);
        }
    }
    free(delivered);
}
#endif

static void paint_notes(struct app_hello *state)
{
    static const gsp_component_key_t rows[] = {
        GSP_NOTES_OBJ_KEY_NOTE_ROW_0, GSP_NOTES_OBJ_KEY_NOTE_ROW_1,
        GSP_NOTES_OBJ_KEY_NOTE_ROW_2, GSP_NOTES_OBJ_KEY_NOTE_ROW_3,
    };
    static const gsp_component_key_t titles[] = {
        GSP_NOTES_OBJ_KEY_NOTE_TITLE_0, GSP_NOTES_OBJ_KEY_NOTE_TITLE_1,
        GSP_NOTES_OBJ_KEY_NOTE_TITLE_2, GSP_NOTES_OBJ_KEY_NOTE_TITLE_3,
    };
    static const gsp_component_key_t metas[] = {
        GSP_NOTES_OBJ_KEY_NOTE_META_0, GSP_NOTES_OBJ_KEY_NOTE_META_1,
        GSP_NOTES_OBJ_KEY_NOTE_META_2, GSP_NOTES_OBJ_KEY_NOTE_META_3,
    };
    static const gsp_component_key_t chevrons[] = {
        GSP_NOTES_OBJ_KEY_NOTE_CHEVRON_0, GSP_NOTES_OBJ_KEY_NOTE_CHEVRON_1,
        GSP_NOTES_OBJ_KEY_NOTE_CHEVRON_2, GSP_NOTES_OBJ_KEY_NOTE_CHEVRON_3,
    };
    static esp_gsp_err_t (*const title_text[])(esp_gsp_handle_t, const char *) = {
        gsp_notes_note_title_0_set_text, gsp_notes_note_title_1_set_text,
        gsp_notes_note_title_2_set_text, gsp_notes_note_title_3_set_text,
    };
    static esp_gsp_err_t (*const meta_text[])(esp_gsp_handle_t, const char *) = {
        gsp_notes_note_meta_0_set_text, gsp_notes_note_meta_1_set_text,
        gsp_notes_note_meta_2_set_text, gsp_notes_note_meta_3_set_text,
    };
    static esp_gsp_err_t (*const chevron_text[])(esp_gsp_handle_t, const char *) = {
        gsp_notes_note_chevron_0_set_text, gsp_notes_note_chevron_1_set_text,
        gsp_notes_note_chevron_2_set_text, gsp_notes_note_chevron_3_set_text,
    };
    char title[72];
    char meta[72];
    char when[40];
    char conclusions[180];
    char todos[320];
    char count[24];
    bool open;
    size_t count_notes;
    if (state->notes_step == 0) {
        if (state->notes_painted == state->notes_revision) {
            return;
        }
        state->notes_step = 1;
    }
    open = meeting_notes_is_open(&state->notes);
    count_notes = meeting_notes_count(&state->notes);
    snprintf(count, sizeof(count), "%u 份", (unsigned)count_notes);
    if (state->notes_step == 1) {
        remember_error(state, gsp_notes_notes_heading_set_text(state->ui, "纪要"));
        remember_error(state, gsp_notes_notes_count_set_text(state->ui, count));
        remember_error(state, gsp_notes_notes_empty_set_text(state->ui, meeting_notes_empty_text()));
        show_note(state, GSP_NOTES_OBJ_KEY_NOTES_HEADING, !open);
        show_note(state, GSP_NOTES_OBJ_KEY_NOTES_COUNT, !open);
        show_note(state, GSP_NOTES_OBJ_KEY_NOTES_EMPTY, !open && count_notes == 0);
    } else if (state->notes_step >= 2 && state->notes_step <= 5) {
        size_t i = (size_t)(state->notes_step - 2);
        bool show = !open && i < count_notes;
        title[0] = meta[0] = '\0';
        if (show) {
            meeting_notes_row(&state->notes, i, title, sizeof(title), meta, sizeof(meta));
        }
        remember_error(state, title_text[i](state->ui, title));
        remember_error(state, meta_text[i](state->ui, meta));
        remember_error(state, chevron_text[i](state->ui, ">"));
        if (show) {
            show_note(state, rows[i], true);
        }
        show_note(state, titles[i], show);
        show_note(state, metas[i], show);
        show_note(state, chevrons[i], show);
    } else if (state->notes_step == 6) {
        /* The page of the conclusion.
         *
         * The to-dos have a page of their own, so the box of the to-dos is
         * the whole screen and a card with four to-dos shows all of them.
         * The button that removes the card is here, because it is about the
         * card and not about the to-dos. */
        meeting_notes_detail(&state->notes, title, sizeof(title), when, sizeof(when), conclusions, sizeof(conclusions),
                             todos, sizeof(todos));
        remember_error(state, gsp_notes_notes_back_mark_set_text(state->ui, "<"));
        remember_error(state, gsp_notes_notes_kicker_set_text(state->ui, "纪要"));
        remember_error(state, gsp_notes_notes_detail_title_set_text(state->ui, title));
        remember_error(state, gsp_notes_notes_detail_when_set_text(state->ui, when));
        remember_error(state, gsp_notes_notes_conclusion_label_set_text(state->ui, "结论"));
        remember_error(state, gsp_notes_notes_detail_conclusions_set_text(state->ui, conclusions));
        remember_error(state, gsp_notes_notes_remove_label_set_text(state->ui, "从设备移除"));
        if (open) {
            remember_error(state, gsp_notes_notes_detail_set_y(state->ui, 0));
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_BACK, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_BACK_MARK, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_KICKER, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_TITLE, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_WHEN, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_CONCLUSION_CARD, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_CONCLUSION_LABEL, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_CONCLUSIONS, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_REMOVE, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_REMOVE_LABEL, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_TODO_CARD, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_TODO_LABEL, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_TODOS, false);
            state->notes_detail_shown = 1;
        } else if (state->notes_detail_shown) {
            remember_error(state, gsp_notes_notes_detail_set_y(state->ui, 1000));
            state->notes_detail_shown = 0;
        }
    } else if (state->notes_step == 7) {
        /* The page of the to-dos.
         *
         * The box of the to-dos is the whole screen here, so every to-do
         * of the card has room. Before, the box held two lines and the rest
         * of the to-dos could not be read at all. */
        meeting_notes_detail(&state->notes, title, sizeof(title), when, sizeof(when), conclusions, sizeof(conclusions),
                             todos, sizeof(todos));
        remember_error(state, gsp_notes_notes_todo_label_set_text(state->ui, "待办"));
        remember_error(state, gsp_notes_notes_detail_todos_set_text(state->ui, todos));
        if (open) {
            remember_error(state, gsp_notes_notes_detail_set_y(state->ui, 0));
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_BACK, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_BACK_MARK, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_KICKER, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_TODO_CARD, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_TODO_LABEL, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_TODOS, true);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_TITLE, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_WHEN, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_CONCLUSION_CARD, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_CONCLUSION_LABEL, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_DETAIL_CONCLUSIONS, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_REMOVE, false);
            show_note(state, GSP_NOTES_OBJ_KEY_NOTES_REMOVE_LABEL, false);
            state->notes_detail_shown = 1;
        } else if (state->notes_detail_shown) {
            remember_error(state, gsp_notes_notes_detail_set_y(state->ui, 1000));
            state->notes_detail_shown = 0;
        }
    } else {
        /* The list starts over here.
         *
         * The card has two pages of its own: the conclusion and the
         * to-dos. The detail stays on the screen while the card is open,
         * and it goes away when the card is closed. Before, this page showed
         * the whole card at once; now the two pages are separate, so this
         * page only has to keep the detail where it is. */
        if (!open && state->notes_detail_shown) {
            remember_error(state, gsp_notes_notes_detail_set_y(state->ui, 1000));
            state->notes_detail_shown = 0;
        }
        state->notes_step = 0;
        state->notes_painted = state->notes_revision;
        return;
    }
    state->notes_step++;
}

static void bump_xiaozhi(struct app_hello *state)
{
    state->xiaozhi_revision++;
}

#if defined(ESP_PLATFORM)
/* The task that owns the work of the channel.
 *
 * The two channels are a choice, so their work must not run at the same
 * time: the board does not have the memory for both, and the cloud must
 * not be asked for the device while the private inbox is in use.
 *
 * The task of the screen cannot do this work itself, because letting the
 * work of a channel end takes a while, and the screen must keep answering.
 * So the screen puts the choice of the channel in a queue, and this task
 * does the work: it lets the work of the channel that is left end, waits
 * for it to be gone, and then starts the work of the channel that is on.
 *
 * The task of the screen does not start any work of a channel either, so
 * the two channels never reach the network at the same time. */
static void channel_task(void *arg)
{
    struct app_hello *state = arg;
    uplink_channel_t current = state->channel;
    if (!state) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        /* The work of the channel that is on. */
        if (current == UPLINK_CHANNEL_XIAOZHI) {
            /* The check-in talks to the public server, so it waits for the
             * address of the network. */
            if (station_has_address() && state->xiaozhi_needs_bind) {
                xiaozhi_bind_start();
            }
        } else if (station_has_address()) {
            /* The private channel looks for the minutes from time to time.
             * The task of the screen does not do it, so the pull of the
             * private channel and the check-in of the cloud never run at
             * the same time. */
            meeting_notes_sync_start();
        }
        /* Wait for the next choice of the user. */
        uplink_channel_t next = current;
        if (xQueueReceive(state->channel_requests, &next, pdMS_TO_TICKS(APP_HELLO_CHANNEL_POLL_MS)) != pdTRUE) {
            continue;
        }
        /* The channel changed. The work of the old channel ends before the
         * work of the new one starts, so the board never holds both. */
        if (current == UPLINK_CHANNEL_XIAOZHI) {
            xiaozhi_bind_stop();
        } else {
            meeting_notes_sync_stop();
        }
        current = next;
        state->work_channel = (uint8_t)current;
        ESP_LOGI("app_hello", "the work of the channel %s is on", uplink_channel_label(current));
        /* The stack of this task is measured, so it can be kept to what the
         * work needs. The internal memory of the board is small, and every
         * kilobyte here is one that the other tasks cannot have.
         *
         * The high water mark of ESP-IDF is in bytes, where the FreeRTOS
         * documentation says words, and it is the space that is still free.
         * The number of the bytes that were used is the difference. */
        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI("app_hello", "the task of the channel used %u bytes of the %u byte stack",
                 (unsigned)APP_HELLO_CHANNEL_STACK - (unsigned)free_bytes,
                 (unsigned)APP_HELLO_CHANNEL_STACK);
    }
    vTaskDelete(NULL);
}
#endif

/* Ask the task of the channel for a change.
 *
 * The change is done by that task, because letting the work of a channel
 * end takes a while and the screen must keep answering. */
static void request_channel(struct app_hello *state, uplink_channel_t channel)
{
#if defined(ESP_PLATFORM)
    if (xQueueSend(state->channel_requests, &channel, 0) != pdTRUE) {
        ESP_LOGE("app_hello", "the change of the channel could not be asked for");
    }
#else
    (void)state;
    (void)channel;
#endif
}

/* Put the device on a channel.
 *
 * The two channels are a choice: one of them is on, and the other one is
 * off. The choice is kept, so the device comes back on it.
 *
 * The screen is told at once, so the user sees the choice he made. The
 * work of the channel changes on its own task, because leaving a channel
 * releases what it owns and that takes a while: the check-in of the cloud
 * has to end before the pull of the minutes starts, or the other way
 * around, so the two channels never hold the board at the same time.
 *
 * The choice is kept on the flash by the store of the channel, which
 * writes it on a task of its own: the interface cannot write the flash
 * itself, because the stack of the render task is in the external memory
 * and the flash is written with the cache of the flash turned off. */
static void switch_channel(struct app_hello *state, uplink_channel_t channel)
{
    if (!state) {
        return;
    }
    /* The name of the task is in the log, so it can be seen that the switch
     * is done by the task of the screen, and that the write of the flash is
     * not done by it. The write of the flash cannot be done by the task of
     * the screen: its stack is in the external memory, and the flash is
     * written with the cache of the flash turned off. */
#if defined(ESP_PLATFORM)
    ESP_LOGI("app_hello", "the channel is switched by the task %s", pcTaskGetName(NULL));
#endif
    if (channel == UPLINK_CHANNEL_XIAOZHI) {
        state->channel = UPLINK_CHANNEL_XIAOZHI;
        state->xiaozhi_open = true;
        paint_channel(state);
        xiaozhi_bind_view_waiting(&state->xiaozhi);
        /* The token of the cloud lives in memory only and goes out of date,
         * so the cloud is asked again when the user comes back to this
         * channel. */
        state->xiaozhi_needs_bind = true;
#if !defined(ESP_PLATFORM)
        /* The simulator has no server. The first tap shows the activation
         * code; after that the device counts as bound, which is what the
         * server answers once the user bound it on xiaozhi.me. */
        if (!state->xiaozhi_sim_bound) {
            (void)xiaozhi_bind_view_from_ota("{\"activation\":{\"code\":\"602245\"}}", &state->xiaozhi);
        } else {
            state->xiaozhi_bound = true;
            snprintf(state->xiaozhi_url, sizeof(state->xiaozhi_url), "%s", "wss://simulator.invalid/v1/");
            snprintf(state->xiaozhi_token, sizeof(state->xiaozhi_token), "%s", "simulator-token");
            state->xiaozhi.show_code = false;
            snprintf(state->xiaozhi.hint, sizeof(state->xiaozhi.hint), "%s", "已绑定");
        }
#endif
        (void)uplink_channel_store(state->channel);
        bump_xiaozhi(state);
        /* The task of the channel lets the work of the private channel end
         * and starts the check-in of the cloud. */
        request_channel(state, state->channel);
        return;
    }
    /* The private channel. */
    state->channel = uplink_channel_default();
    state->xiaozhi_open = false;
    (void)uplink_channel_store(state->channel);
    bump_xiaozhi(state);
    paint_channel(state);
    /* The task of the channel lets the check-in of the cloud end, so the
     * task that talks to the public server does not hold on to its socket
     * and its buffer while the private channel works. */
    request_channel(state, state->channel);
}

/* Show which channel the device is on.
 *
 * The device was on a channel the user could not see, so a recording went
 * to one server while the user thought it went to another. The capsule next
 * to the wifi status carries the channel, and the settings page carries the
 * choice between the two channels. */
static void paint_channel(struct app_hello *state)
{
    ESP_LOGI("app_hello", "the channel is %s", uplink_channel_label(state->channel));
    remember_error(state, gsp_recorder_channel_text_set_text(state->ui, uplink_channel_label(state->channel)));
}

/* The message the panel shows under the rows. */
static void set_files_message(struct app_hello *state, const char *text)
{
    remember_error(state, gsp_recorder_files_confirm_set_text(state->ui, text));
}

/* Say something on the screen the user is looking at.
 *
 * The recordings panel covers the main screen, so a message that only goes
 * to the main screen is a message nobody reads. The upload refused to start
 * while a recording was running, and the panel said nothing about it, so
 * the button looked like it had done nothing at all. */
static void set_upload_message(struct app_hello *state, const char *text)
{
    remember_error(state, gsp_recorder_sub_text_set_text(state->ui, text));
    if (state->files_visible) {
        set_files_message(state, text);
    }
}


/* The Xiaozhi binding on the settings page.
 *
 * The binding has its own section there, next to the channel and the
 * inbox, so the code and what to do with it are on the same page as
 * everything else the device can be told to do. */
static void paint_xiaozhi(struct app_hello *state)
{
    const char *code;
    const char *hint;

    if (!state || state->scene_id != 2) {
        return;
    }
    if (state->xiaozhi_painted == state->xiaozhi_revision) {
        return;
    }
    if (uplink_channel_runs_xiaozhi(state->channel)) {
        code = state->xiaozhi.show_code ? state->xiaozhi.code : "";
        hint = state->xiaozhi.hint;
        if (state->xiaozhi_bound) {
            hint = "已绑定";
        }
    } else {
        code = "";
        hint = "小智通道未启用";
    }
    remember_error(state, gsp_settings_xz_code_set_text(state->ui, code[0] ? code : "------"));
    remember_error(state, gsp_settings_xz_hint_set_text(state->ui, hint));
    state->xiaozhi_painted = state->xiaozhi_revision;
}

static void on_ui_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    struct app_hello *state = ctx;
    if (!state || !event) {
        return;
    }
    if (event->type == ESP_GSP_EVENT_SCENE_CHANGED) {
        state->scene_id = event->scene_id;
        if (event->scene_id == 1) {
            bump_notes(state);
        }
        /* The channel was changed on the settings page, and the text of the
         * capsule on the main screen was written while that page was on the
         * screen, so the main screen never saw it. It says again what it
         * shows when it comes back. */
        paint_channel(state);
        return;
    }
    if (gsp_notes_event_is_on_note_0(event) || gsp_notes_event_is_on_note_1(event) ||
        gsp_notes_event_is_on_note_2(event) || gsp_notes_event_is_on_note_3(event)) {
        size_t index = gsp_notes_event_is_on_note_1(event) ? 1 :
                       gsp_notes_event_is_on_note_2(event) ? 2 :
                       gsp_notes_event_is_on_note_3(event) ? 3 : 0;
        if (meeting_notes_open(&state->notes, index)) {
            bump_notes(state);
        }
        return;
    }
    if (gsp_notes_event_is_on_notes_back(event)) {
        meeting_notes_close(&state->notes);
        bump_notes(state);
        return;
    }
    if (gsp_notes_event_is_on_notes_remove(event)) {
        if (meeting_notes_remove_open(&state->notes)) {
            meeting_notes_save(&state->notes, MEETING_NOTES_PATH);
            bump_notes(state);
        }
        return;
    }
    if (gsp_settings_event_is_on_channel_private(event)) {
        switch_channel(state, uplink_channel_default());
        return;
    }
    if (gsp_settings_event_is_on_channel_xiaozhi(event)) {
        switch_channel(state, UPLINK_CHANNEL_XIAOZHI);
        return;
    }
    if (gsp_recorder_event_is_on_record(event)) {
        toggle_recording(ui, state);
    } else if (gsp_recorder_event_is_on_upload(event)) {
        request_upload(ui, state);
    } else if (gsp_recorder_event_is_on_files(event) || gsp_recorder_event_is_on_files_back(event)) {
        if (gsp_recorder_event_is_on_files_back(event) || state->files_visible) {
            recorder_files_init(&state->files);
            set_files_visible(state, false);
        } else {
            /* The panel is shown right away with the rows that are already
             * known. The tree is read on its own task, because reading it
             * takes seconds and the screen must not wait for that. */
            remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Tap a row"));
            remember_error(state, gsp_recorder_files_multi_label_set_text(ui, "Multi"));
            set_files_visible(state, true);
#if defined(ESP_PLATFORM)
            files_scan_request(state);
#else
            reload_file_rows(state);
#endif
            refresh_file_list(state);
        }
    } else if (gsp_recorder_event_is_on_files_multi(event)) {
        (void)recorder_files_set_multi(&state->files, !state->files.multi);
        remember_error(state, gsp_recorder_files_multi_label_set_text(ui, state->files.multi ? "Multi on" : "Multi"));
        remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Tap a row"));
        refresh_file_list(state);
    } else if (files_row_from_event(event) >= 0) {
        const recorder_file_row_t *file = files_row_at(state, (size_t)files_row_from_event(event));
        if (file) {
            (void)recorder_files_tap(&state->files, state->files_library, file->session_id);
            remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Tap a row"));
            refresh_file_list(state);
        }
    } else if (gsp_recorder_event_is_on_files_page_prev(event)) {
        if (state->files_page > 0) {
            state->files_page--;
            refresh_file_list(state);
        }
    } else if (gsp_recorder_event_is_on_files_page_next(event)) {
        if ((size_t)state->files_page + 1u < files_page_count(state)) {
            state->files_page++;
            refresh_file_list(state);
        }
    } else if (inbox_row_from_event(event) >= 0) {
#if defined(ESP_PLATFORM)
        /* A tap on a row is the user saying that this is the inbox to use.
         * The address is kept, so it survives a restart. */
        if (inbox_discovery_confirm((size_t)inbox_row_from_event(event)) == 0) {
            paint_settings(state);
        }
#endif
    } else if (wifi_key_from_event(event) >= 0) {
        (void)wifi_picker_tap(&state->wifi, (size_t)wifi_key_from_event(event));
        mark_wifi_dirty(state);
    } else if (gsp_recorder_event_is_on_wifi_0(event) || gsp_recorder_event_is_on_wifi_1(event) ||
               gsp_recorder_event_is_on_wifi_2(event) || gsp_recorder_event_is_on_wifi_3(event) ||
               gsp_recorder_event_is_on_wifi_4(event) || gsp_recorder_event_is_on_wifi_5(event) ||
               gsp_recorder_event_is_on_wifi_6(event) || gsp_recorder_event_is_on_wifi_7(event)) {
        int row = gsp_recorder_event_is_on_wifi_1(event) ? 1 :
                  gsp_recorder_event_is_on_wifi_2(event) ? 2 :
                  gsp_recorder_event_is_on_wifi_3(event) ? 3 :
                  gsp_recorder_event_is_on_wifi_4(event) ? 4 :
                  gsp_recorder_event_is_on_wifi_5(event) ? 5 :
                  gsp_recorder_event_is_on_wifi_6(event) ? 6 :
                  gsp_recorder_event_is_on_wifi_7(event) ? 7 : 0;
        if (wifi_picker_tap(&state->wifi, (size_t)row) == 1) {
            queue_wifi_result(state, true);
        }
        mark_wifi_dirty(state);
    } else if (gsp_recorder_event_is_on_wifi_scan(event)) {
        if (state->wifi.password_mode) {
            wifi_picker_next_page(&state->wifi);
            mark_wifi_dirty(state);
        } else {
            queue_wifi_result(state, false);
        }
    } else if (gsp_recorder_event_is_on_wifi_del(event)) {
        wifi_picker_backspace(&state->wifi);
        mark_wifi_dirty(state);
    } else if (gsp_recorder_event_is_on_wifi_join(event)) {
        queue_wifi_result(state, true);
        mark_wifi_dirty(state);
    } else if (gsp_recorder_event_is_on_wifi_close(event)) {
        (void)esp_gsp_drawer_close(ui, GSP_RECORDER_OBJ_KEY_WIFI_PANEL, true);
    } else if (gsp_recorder_event_is_on_setup(event)) {
        reopen_provision(state);
    } else if (gsp_recorder_event_is_on_files_upload(event)) {
        uint32_t sessions[APP_HELLO_UPLOAD_SESSIONS];
        size_t count = 0;
        size_t total = recorder_library_count(state->files_library);
        for (size_t i = 0; i < total && count < APP_HELLO_UPLOAD_SESSIONS; i++) {
            const recorder_file_row_t *row = recorder_library_at(state->files_library, i);
            if (row && row->selected && !row->open) {
                sessions[count++] = row->session_id;
            }
        }
        if (count == 0) {
            remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Select a saved file"));
        } else {
            char prompt[32];
            /* The upload takes the label away from the delete confirmation, so
             * that confirmation is dropped. A delete that follows has to ask
             * for its count again. */
            (void)recorder_files_set_multi(&state->files, state->files.multi);
            snprintf(prompt, sizeof(prompt), "Uploading %u", (unsigned)count);
            set_files_message(state, prompt);
            request_upload_sessions(ui, state, sessions, count);
        }
    } else if (gsp_recorder_event_is_on_files_delete(event)) {
        char prompt[32];
        if (!state->files.confirm_pending) {
            if (recorder_files_request_delete(&state->files, state->files_library, prompt, sizeof(prompt)) == 0) {
                remember_error(state, gsp_recorder_files_confirm_set_text(ui, prompt));
            } else {
                remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Select a saved file"));
            }
        } else {
#if defined(ESP_PLATFORM)
            size_t removed = 0;
            recorder_store_t *store = recorder_store_open("/nand/recordings");
            if (store && recorder_files_confirm_delete(&state->files, state->files_library, store, &removed) == 0) {
                refresh_inventory(state);
                render_inventory(ui, state);
            }
            if (store) {
                recorder_store_close(store);
            }
            remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Tap a row"));
            refresh_file_list(state);
#else
            state->files.confirm_pending = false;
            remember_error(state, gsp_recorder_files_confirm_set_text(ui, "Saved on device"));
#endif
        }
    }
}

#if !defined(ESP_PLATFORM)
/* The simulator has no socket. It walks the same phases so the screen text
 * and the card page can be checked before device work. */
static void simulate_xiaozhi_upload(esp_gsp_handle_t ui, struct app_hello *state)
{
    static const char card[] =
        "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通\nconclusion 下周再确认范围\ntodo 发送纪要\n";
    if (!state->xiaozhi_sim_step) {
        return;
    }
    state->xiaozhi_sim_step++;
    if (state->xiaozhi_sim_step == 1) {
        state->xiaozhi_phase = XIAOZHI_NOTE_UPLOADING;
    } else if (state->xiaozhi_sim_step == 2) {
        state->xiaozhi_phase = XIAOZHI_NOTE_SUMMARIZING;
    } else if (state->xiaozhi_sim_step == 3) {
        if (meeting_notes_merge_document(&state->notes, card) == 0) {
            meeting_notes_save(&state->notes, MEETING_NOTES_PATH);
            bump_notes(state);
        }
        state->xiaozhi_phase = XIAOZHI_NOTE_RECEIVED;
    } else {
        state->xiaozhi_sim_step = 0;
        return;
    }
    remember_error(state, gsp_recorder_sub_text_set_text(ui, xiaozhi_note_phase_text(state->xiaozhi_phase)));
}
#endif

#if !defined(ESP_PLATFORM)
static void wifi_demo_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    bool open = false;
    wifi_network_t demo[] = {
        {.ssid = "home", .rssi = -40, .open = false},
        {.ssid = "cafe", .rssi = -55, .open = true},
    };
    if (!state) {
        return;
    }
#if !defined(ESP_PLATFORM)
    simulate_xiaozhi_upload(ui, state);
#endif
    if (state->scene_id != 0) {
        paint_notes(state);
        return;
    }
    paint_xiaozhi(state);
    if (esp_gsp_drawer_is_open(ui, GSP_RECORDER_OBJ_KEY_WIFI_PANEL, &open) != ESP_GSP_OK) {
        return;
    }
    wifi_sheet_note(&state->wifi_sheet, open ? 1 : 0, false, true);
    if (wifi_sheet_take_closed(&state->wifi_sheet)) {
        wifi_picker_show_networks(&state->wifi);
        mark_wifi_dirty(state);
    }
    if (wifi_sheet_take_scan(&state->wifi_sheet)) {
        wifi_picker_set_scan(&state->wifi, demo, 2);
        snprintf(state->link_ssid, sizeof(state->link_ssid), "home");
        mark_wifi_dirty(state);
    }
    paint_wifi(state);
    paint_wifi_link(state);
}
#endif

esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;
    state->state = APP_HELLO_STATE_IDLE;
    state->error = APP_HELLO_ERROR_NONE;
    state->xiaozhi_version = 1;
    state->files_library = recorder_library_create();
    recorder_files_init(&state->files);
    provision_screen_init(&state->provision);
    wifi_picker_init(&state->wifi);
    wifi_sheet_init(&state->wifi_sheet);
    if (!state->files_library) {
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    /* The device comes back on the channel it was on. The channel is read
     * before the task of the channel is made, because that task starts the
     * work of the channel the device was on. */
    uplink_channel_load(&state->channel);
    state->work_channel = (uint8_t)state->channel;
#if defined(ESP_PLATFORM)
    recorder_async_init(&state->upload_job);
    /* The recordings tree is read on its own task, because the read takes
     * seconds and the screen must keep answering while it runs.
     *
     * The task runs below the screen. At the same priority the read took
     * the CPU away from the screen: the display needed 453 ms for a frame
     * and reported that it could not find a window to draw in, so the screen
     * stopped repainting while the read ran. */
    if (xTaskCreate(files_scan_task, "files_scan", 6144, state, 1, NULL) != pdPASS) {
        ESP_LOGE("app_hello", "the recordings scan task could not be created");
    }
    state->upload_results = xQueueCreate(1, sizeof(upload_result_t));
    state->command_requests = xQueueCreate(4, sizeof(app_hello_request_t));
    state->recorder_results = xQueueCreate(1, sizeof(int));
    state->provision_payloads = xQueueCreate(2, sizeof(char *));
    state->provision_finished = xQueueCreate(1, sizeof(uint8_t));
    state->wifi_offers = xQueueCreate(1, sizeof(wifi_offer_t));
    state->wifi_results = xQueueCreate(1, sizeof(wifi_result_t));
    state->channel_requests = xQueueCreate(4, sizeof(uplink_channel_t));
    if (!state->upload_results || !state->command_requests || !state->recorder_results ||
        !state->provision_payloads ||
        !state->provision_finished || !state->wifi_offers || !state->wifi_results ||
        !state->channel_requests) {
        if (state->upload_results) {
            vQueueDelete(state->upload_results);
        }
        if (state->command_requests) {
            vQueueDelete(state->command_requests);
        }
        if (state->recorder_results) {
            vQueueDelete(state->recorder_results);
        }
        if (state->provision_payloads) {
            vQueueDelete(state->provision_payloads);
        }
        if (state->provision_finished) {
            vQueueDelete(state->provision_finished);
        }
        if (state->wifi_offers) {
            vQueueDelete(state->wifi_offers);
        }
        if (state->wifi_results) {
            vQueueDelete(state->wifi_results);
        }
        if (state->channel_requests) {
            vQueueDelete(state->channel_requests);
        }
        recorder_library_destroy(state->files_library);
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    /* The task of the channel owns the work of the channel. The device
     * comes back on the channel it was on, and that channel's work starts
     * on that task. */
    state->xiaozhi_needs_bind = true;
    if (xTaskCreate(channel_task, "channel", APP_HELLO_CHANNEL_STACK, state, APP_HELLO_CHANNEL_PRIO, NULL) != pdPASS) {
        ESP_LOGE("app_hello", "the task of the channel could not be created");
        vQueueDelete(state->upload_results);
        vQueueDelete(state->command_requests);
        vQueueDelete(state->recorder_results);
        vQueueDelete(state->provision_payloads);
        vQueueDelete(state->provision_finished);
        vQueueDelete(state->wifi_offers);
        vQueueDelete(state->wifi_results);
        vQueueDelete(state->channel_requests);
        recorder_library_destroy(state->files_library);
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    refresh_inventory(state);
    state->upload_timer = esp_gsp_timer_create(ui, 200, upload_tick, state);
    if (!state->upload_timer) {
        vQueueDelete(state->upload_results);
        vQueueDelete(state->command_requests);
        vQueueDelete(state->provision_payloads);
        vQueueDelete(state->provision_finished);
        vQueueDelete(state->wifi_offers);
        vQueueDelete(state->wifi_results);
        vQueueDelete(state->channel_requests);
        recorder_library_destroy(state->files_library);
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
#endif

    remember_error(state, gsp_recorder_status_text_set_text(ui, "IDLE"));
    remember_error(state, gsp_recorder_timer_set_text(ui, "00:00"));
    remember_error(state, gsp_recorder_sub_text_set_text(ui, "Tap Record to start"));
    remember_error(state, gsp_recorder_btn_record_label_set_text(ui, "Record"));
    paint_channel(state);
    render_inventory(ui, state);
    paint_wifi_link(state);
#if defined(ESP_PLATFORM)
    if (meeting_notes_load(&state->notes, MEETING_NOTES_PATH) != 0) {
        meeting_notes_init(&state->notes);
    }
#else
    {
        const char *review[] = {"确认左滑进入纪要", "纪要写入 Obsidian 后下发"};
        const char *client[] = {"发送纪要", "约下次会议"};
        const char *weekly[] = {"整理录音"};
        meeting_notes_merge_card(&state->notes, "review", "2026-11-11 15:20", "产品评审会", "纪要放到设备上阅读。",
                                 review, 2);
        meeting_notes_merge_card(&state->notes, "client", "2026-11-10 08:30", "客户沟通", "下周再确认范围。", client, 2);
        meeting_notes_merge_card(&state->notes, "weekly", "2026-11-09 09:00", "周会", "本周继续。", weekly, 1);
    }
#endif

    esp_gsp_err_t err = state->last_error;
    if (err == ESP_GSP_OK) {
        err = esp_gsp_on_event(ui, on_ui_event, state);
    }
#if !defined(ESP_PLATFORM)
    if (err == ESP_GSP_OK && !esp_gsp_timer_create(ui, 100, wifi_demo_tick, state)) {
        err = ESP_GSP_ERR_NO_MEM;
    }
#endif
    if (err != ESP_GSP_OK) {
#if defined(ESP_PLATFORM)
        (void)esp_gsp_timer_delete(ui, state->upload_timer);
        vQueueDelete(state->upload_results);
        vQueueDelete(state->command_requests);
        vQueueDelete(state->provision_payloads);
        vQueueDelete(state->provision_finished);
        vQueueDelete(state->wifi_offers);
        vQueueDelete(state->wifi_results);
        vQueueDelete(state->channel_requests);
#endif
        recorder_library_destroy(state->files_library);
        free(state);
        return err;
    }
    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_offer_wifi(app_hello_handle_t handle, const wifi_network_t *networks, size_t count)
{
#if defined(ESP_PLATFORM)
    wifi_offer_t offer = {0};
    wifi_result_t stale;
    if (!handle || (!networks && count > 0) || count > sizeof(offer.networks) / sizeof(offer.networks[0])) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (count > 0) {
        memcpy(offer.networks, networks, count * sizeof(offer.networks[0]));
    }
    offer.count = count;
    while (xQueueReceive(handle->wifi_results, &stale, 0) == pdTRUE) {
    }
    return xQueueOverwrite(handle->wifi_offers, &offer) == pdTRUE ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
#else
    (void)handle;
    (void)networks;
    (void)count;
    return ESP_GSP_ERR_NOT_SUPPORTED;
#endif
}

app_hello_wifi_action_t app_hello_poll_wifi(app_hello_handle_t handle, char *ssid, size_t ssid_cap, char *password,
                                         size_t password_cap, uint32_t timeout_ms)
{
#if defined(ESP_PLATFORM)
    wifi_result_t result;
    if (!handle || !ssid || !password || ssid_cap < 33 || password_cap < 64) {
        return APP_HELLO_WIFI_TIMEOUT;
    }
    if (xQueueReceive(handle->wifi_results, &result, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return APP_HELLO_WIFI_TIMEOUT;
    }
    if (!result.join) {
        return APP_HELLO_WIFI_RESCAN;
    }
    snprintf(ssid, ssid_cap, "%s", result.ssid);
    snprintf(password, password_cap, "%s", result.password);
    return APP_HELLO_WIFI_JOIN;
#else
    (void)handle;
    (void)ssid;
    (void)ssid_cap;
    (void)password;
    (void)password_cap;
    (void)timeout_ms;
    return APP_HELLO_WIFI_TIMEOUT;
#endif
}

esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot)
{
    if (!handle || !out_snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_hello_snapshot_t) {
        .session_count = handle->session_count,
        .pending_count = handle->pending_count,
        .notes_count = meeting_notes_count(&handle->notes),
        .current_elapsed_s = handle->current_elapsed_s,
        .recording = handle->recording,
        .state = handle->state,
        .error = handle->error,
        .last_error = handle->last_error,
        .channel = uplink_channel_runs_xiaozhi(handle->channel) ? APP_HELLO_CHANNEL_XIAOZHI
                                                               : APP_HELLO_CHANNEL_PRIVATE,
        .work_channel = (handle->work_channel == (uint8_t)UPLINK_CHANNEL_XIAOZHI) ? APP_HELLO_CHANNEL_XIAOZHI
                                                                                  : APP_HELLO_CHANNEL_PRIVATE,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_switch_channel(app_hello_handle_t handle, app_hello_channel_t channel)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
#if defined(ESP_PLATFORM)
    /* The switch is done by the task of the screen, like the other actions
     * of the interface. The caller is on another task, and the screen is
     * not to be touched from two tasks at once. */
    app_hello_request_t request = {
        .command = APP_HELLO_COMMAND_CHANNEL,
        .channel = channel,
    };
    return xQueueSend(handle->command_requests, &request, 0) == pdTRUE ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
#else
    switch_channel(handle, channel == APP_HELLO_CHANNEL_XIAOZHI ? UPLINK_CHANNEL_XIAOZHI : UPLINK_CHANNEL_PRIVATE);
    return ESP_GSP_OK;
#endif
}

esp_gsp_err_t app_hello_request(app_hello_handle_t handle, app_hello_command_t command)
{
    if (!handle || command < APP_HELLO_COMMAND_TOGGLE_RECORD || command > APP_HELLO_COMMAND_UPLOAD) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
#if defined(ESP_PLATFORM)
    app_hello_request_t request = {
        .command = command,
        .channel = APP_HELLO_CHANNEL_PRIVATE,
    };
    return xQueueSend(handle->command_requests, &request, 0) == pdTRUE ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
#else
    if (command == APP_HELLO_COMMAND_TOGGLE_RECORD) {
        toggle_recording(handle->ui, handle);
    } else {
        request_upload(handle->ui, handle);
    }
    return ESP_GSP_OK;
#endif
}

esp_gsp_err_t app_hello_offer_provision(app_hello_handle_t handle, const char *payload)
{
    if (!handle || !payload || payload[0] == '\0' || strlen(payload) >= PROVISION_SCREEN_PAYLOAD_MAX) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
#if defined(ESP_PLATFORM)
    char *copy = strdup(payload);
    if (!copy) {
        return ESP_GSP_ERR_NO_MEM;
    }
    if (xQueueSend(handle->provision_payloads, &copy, 0) != pdTRUE) {
        free(copy);
        return ESP_GSP_ERR_NO_MEM;
    }
    return ESP_GSP_OK;
#else
    present_provision(handle, payload);
    return ESP_GSP_OK;
#endif
}

esp_gsp_err_t app_hello_finish_provision(app_hello_handle_t handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
#if defined(ESP_PLATFORM)
    uint8_t finished = 1;
    return xQueueOverwrite(handle->provision_finished, &finished) == pdTRUE ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
#else
    finish_provision(handle);
    return ESP_GSP_OK;
#endif
}

bool app_hello_provision_visible(app_hello_handle_t handle)
{
    return handle && handle->provision.visible;
}

bool app_hello_provision_deferred(app_hello_handle_t handle)
{
    return handle && handle->provision.deferred;
}

esp_gsp_err_t app_hello_set_observer(app_hello_handle_t handle, app_hello_observer_t observer, void *ctx)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    handle->observer = observer;
    handle->observer_ctx = ctx;
    notify_observer(handle);
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = *handle;
    if (!state) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(state->ui, NULL, NULL);
#if defined(ESP_PLATFORM)
    if (state->upload_job.running) {
        upload_result_t completed;
        if (xQueueReceive(state->upload_results, &completed, portMAX_DELAY) == pdTRUE) {
            int ignored;
            recorder_async_complete(&state->upload_job, completed.sent);
            (void)recorder_async_take_result(&state->upload_job, &ignored);
        }
    }
    if (state->upload_timer) {
        const esp_gsp_err_t upload_timer_err = esp_gsp_timer_delete(state->ui, state->upload_timer);
        if (err == ESP_GSP_OK) {
            err = upload_timer_err;
        }
    }
    vQueueDelete(state->upload_results);
    vQueueDelete(state->command_requests);
    {
        char *payload = NULL;
        while (xQueueReceive(state->provision_payloads, &payload, 0) == pdTRUE) {
            free(payload);
        }
    }
    vQueueDelete(state->provision_payloads);
    vQueueDelete(state->provision_finished);
    vQueueDelete(state->wifi_offers);
    vQueueDelete(state->wifi_results);
    vQueueDelete(state->channel_requests);
#endif
    if (state->timer) {
        const esp_gsp_err_t timer_err = esp_gsp_timer_delete(state->ui, state->timer);
        if (err == ESP_GSP_OK) {
            err = timer_err;
        }
    }
    if (err != ESP_GSP_OK) {
        return err;
    }
    recorder_library_destroy(state->files_library);
    free(state);
    *handle = NULL;
    return ESP_GSP_OK;
}
