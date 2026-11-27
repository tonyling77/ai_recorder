#include "meeting_notes_sync.h"

#include "app_clock.h"
#include "recorder_upload.h"
#include "recorder_upload_config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "notes_sync";

/* The HTTPS + TLS + lwIP DNS chain reached from esp_http_client_open()
 * needs more than the 8 KB this task used to get: the stack overflowed
 * inside netconn_gethostbyname and the board rebooted in a loop. */
#define NOTES_SYNC_STACK 16384

/* The notes body is read into a heap buffer so it does not eat the stack. */
#define NOTES_SYNC_BODY_CAP 8192

/* How long the sync waits for the wall clock before giving up.
 *
 * The board joins the network a few seconds before SNTP answers, so the
 * first tick after the address arrives is normally too early. The sync is a
 * once-per-boot one-shot, so failing there would mean the notes never
 * download at all. Wait for the clock instead, but not forever: a board that
 * never syncs must still give up and leave the task behind. */
#define NOTES_SYNC_CLOCK_WAIT_MS 60000
#define NOTES_SYNC_CLOCK_POLL_MS 500

/* How long the device waits between two pulls of the notes.
 *
 * The minutes are made on the server after a recording arrives: the server
 * transcribes the audio and writes the card, which takes a moment. A pull
 * that happens right after the upload finds nothing, because the server is
 * still working. The device therefore looks again, and again, until the card
 * is there. */
#define NOTES_SYNC_INTERVAL_MS 60000

static QueueHandle_t s_notes;
static TaskHandle_t s_sync_task;
static int64_t s_last_start_us;
/* Whether the pull has to end.
 *
 * The pull belongs to the private channel. When the user chooses the
 * Xiaozhi channel, the pull has to end, so the inbox is not asked for
 * minutes while the cloud is in use, and the memory of the pull is free
 * for the work of the other channel. */
static volatile bool s_stop;

/* How long the stop waits for the pull to end.
 *
 * The pull ends when the request it is in returns, so the wait is as long
 * as that request can take, plus the wait for the clock.
 *
 * The wait is done by the task that switches the channel, not by the task
 * of the screen: the screen must not stand still while the other channel
 * lets go of what it holds. */
#define NOTES_SYNC_STOP_WAIT_MS (NOTES_SYNC_CLOCK_WAIT_MS + RECORDER_UPLOAD_TIMEOUT_MS + 2000)
#define NOTES_SYNC_STOP_POLL_MS 50

/* Wait until the wall clock is usable, so the TLS handshake can succeed.
 * Returns 0 when the clock is set, -1 when the wait ran out. */
static int wait_for_clock(void)
{
    uint32_t waited_ms = 0;
    while (!app_clock_valid()) {
        if (s_stop) {
            app_clock_log("notes sync gave up waiting for the clock: the channel changed");
            return -1;
        }
        if (waited_ms >= NOTES_SYNC_CLOCK_WAIT_MS) {
            app_clock_log("notes sync gave up waiting for the clock");
            ESP_LOGE(TAG, "download failed: the clock is not set");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTES_SYNC_CLOCK_POLL_MS));
        waited_ms += NOTES_SYNC_CLOCK_POLL_MS;
    }
    return 0;
}

static int download(char *out, size_t cap)
{
    recorder_upload_endpoint_t endpoint;
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;
    int total = 0;
    if (!out || cap < 2 || recorder_upload_endpoint("/v1/notes", &endpoint) != 0) {
        ESP_LOGE(TAG, "download failed: the notes endpoint is not usable");
        return -1;
    }
    /* The certificate bundle checks the validity window of the server
     * certificate against the system clock, so a device without a wall clock
     * cannot do the handshake. A plain HTTP link has no handshake, so it
     * does not need the clock. */
    if (!endpoint.plain_http && wait_for_clock() != 0) {
        return -1;
    }
    esp_http_client_config_t config = {
        .url = endpoint.url,
        .method = HTTP_METHOD_GET,
        /* The same timeout the slice upload uses, so a dead link fails the
         * notes sync in the same time it fails an upload. */
        .timeout_ms = RECORDER_UPLOAD_TIMEOUT_MS,
        /* The certificate bundle belongs to a link that has a certificate. */
        .crt_bundle_attach = endpoint.plain_http ? NULL : esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "download failed: the HTTP client could not be created");
        return -1;
    }
    esp_http_client_set_header(client, endpoint.token_header, RECORDER_UPLOAD_TOKEN);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "download %s failed: %s", endpoint.url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (status != 200) {
        if (status == 404) {
            /* The server has no card for this device yet. That is the normal
             * answer until the minutes are made, so it is not a failure. */
            ESP_LOGI(TAG, "download %s: the server has no notes yet", endpoint.url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return 1;
        }
        ESP_LOGE(TAG, "download %s failed: status=%d", endpoint.url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    while (status == 200 && total < (int)cap - 1) {
        int read = esp_http_client_read(client, out + total, cap - 1 - (size_t)total);
        if (read <= 0) {
            break;
        }
        total += read;
    }
    out[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (total <= 0) {
        /* An empty document is the answer of an inbox that has no card for
         * this device yet, the same answer as a 404. It is not a failure:
         * the minutes are made on the server after the recording arrives, so
         * the device looks again. */
        ESP_LOGI(TAG, "download %s: the server has no notes yet", endpoint.url);
        return 1;
    }
    ESP_LOGI(TAG, "download %s ok: status=%d bytes=%d", endpoint.url, status, total);
    return 0;
}

static void sync_task(void *arg)
{
    char *body = malloc(NOTES_SYNC_BODY_CAP);
    (void)arg;
    ESP_LOGI(TAG, "sync task started");
    if (!body) {
        ESP_LOGE(TAG, "sync failed: the notes buffer could not be allocated");
        s_sync_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (s_stop) {
        /* The channel changed while the task was being made. The pull is
         * not wanted any more, so it does not start. */
        ESP_LOGI(TAG, "sync ended: the channel changed before the pull started");
        free(body);
        s_sync_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int result = download(body, NOTES_SYNC_BODY_CAP);
    if (s_stop) {
        /* The pull ended in the middle of the request. The minutes that
         * came in are dropped: the other channel is in use now. */
        ESP_LOGI(TAG, "sync ended: the channel changed during the pull");
        result = 1;
    }
    if (result == 0) {
        if (s_notes && xQueueSend(s_notes, &body, 0) == pdTRUE) {
            body = NULL;
        } else {
            ESP_LOGW(TAG, "sync ended: the notes queue was full");
        }
    } else if (result == 1) {
        ESP_LOGI(TAG, "sync ended: the server has no notes yet");
    } else {
        ESP_LOGE(TAG, "sync ended: the notes could not be downloaded");
    }
    free(body);
    /* The task owns the single in-flight sync: drop the handle so no stale
     * pointer is kept, and always leave through vTaskDelete so a failed DNS
     * or TLS run can never park the task or take the board down. */
    s_sync_task = NULL;
    vTaskDelete(NULL);
}

void meeting_notes_sync_start(void)
{
    int64_t now = esp_timer_get_time();
    if (s_sync_task) {
        /* One sync in flight. */
        return;
    }
    if (s_last_start_us != 0 && now - s_last_start_us < (int64_t)NOTES_SYNC_INTERVAL_MS * 1000) {
        /* The last pull was recent. The next one is due later. */
        return;
    }
    if (!s_notes) {
        s_notes = xQueueCreate(1, sizeof(char *));
    }
    if (!s_notes) {
        ESP_LOGE(TAG, "the notes queue could not be created");
        return;
    }
    /* The flag is only cleared for a pull that is really started: a pull
     * that is still ending has to end, or the two channels would ask the
     * network at the same time. */
    s_stop = false;
    if (xTaskCreate(sync_task, "notes_sync", NOTES_SYNC_STACK, NULL, 4, &s_sync_task) != pdPASS) {
        s_sync_task = NULL;
        ESP_LOGE(TAG, "the sync task could not be created");
        return;
    }
    s_last_start_us = now;
}

void meeting_notes_sync_stop(void)
{
    if (!s_sync_task) {
        s_stop = true;
        return;
    }
    /* The pull is asked to end and the caller waits for it, so the memory
     * of the pull is free before the other channel starts its work. The
     * board does not have the memory for both channels at once. */
    s_stop = true;
    for (int waited = 0; waited < NOTES_SYNC_STOP_WAIT_MS && s_sync_task; waited += NOTES_SYNC_STOP_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(NOTES_SYNC_STOP_POLL_MS));
    }
    if (s_sync_task) {
        ESP_LOGW(TAG, "the notes sync did not end in time; it will end by itself");
    }
}

char *meeting_notes_sync_take(void)
{
    char *body = NULL;
    if (!s_notes || xQueueReceive(s_notes, &body, 0) != pdTRUE) {
        return NULL;
    }
    return body;
}
