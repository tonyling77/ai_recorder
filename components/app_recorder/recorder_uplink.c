#include "recorder_uplink.h"

#include "app_clock.h"
#include "recorder_store.h"
#include "recorder_store_lock.h"
#include "recorder_upload.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recorder_upload_config.h"

static const char *TAG = "recorder_uplink";
static const char *s_failure;
/* Why nothing was sent, when nothing was sent. */
static const char *s_note;

/* How long the upload waits for the recordings lock.
 *
 * The upload is background work, so it can wait while the screen keeps
 * answering. The listing that runs when the panel opens reads the same
 * tree, and a long recording takes seconds to read on this flash. A short
 * wait turned that into "Storage unavailable" while the listing was still
 * reading. */
#define RECORDER_UPLOAD_LOCK_TIMEOUT_MS 30000

static int post_request(const recorder_upload_request_t *request, const uint8_t *body, size_t len, int *status_out)
{
    recorder_upload_endpoint_t endpoint;
    char session[16];
    char index[8];
    char duration[16];
    char last[4];
    int64_t started_us;
    if (recorder_upload_endpoint(request->path, &endpoint) != 0 || endpoint.token_in_url ||
        (!endpoint.plain_http && (!endpoint.cert_bundle || endpoint.insecure))) {
        ESP_LOGE(TAG, "post %s failed: the upload endpoint is not usable", request->path);
        return -1;
    }
    /* RECORDER_UPLOAD_ORIGIN is the base URL; esp_http_client uses its host as TLS SNI.
     *
     * The health check gets the short timeout so a dead link is reported in
     * seconds. A slice POST carries megabytes and keeps the long budget, so
     * a slow but working WAN link still completes. */
    esp_http_client_config_t config = {
        .url = endpoint.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = request->kind == RECORDER_UPLOAD_SLICE ? endpoint.slice_timeout_ms
                                                             : endpoint.timeout_ms,
        /* The certificate bundle belongs to a link that has a certificate.
         * A plain HTTP link has no handshake, so the bundle stays off it. */
        .crt_bundle_attach = endpoint.plain_http ? NULL : esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "post %s failed: the HTTP client could not be created", request->path);
        return -1;
    }
    esp_http_client_set_header(client, endpoint.token_header, RECORDER_UPLOAD_TOKEN);
    if (request->kind == RECORDER_UPLOAD_HEALTH) {
        esp_http_client_set_header(client, "X-Device-Id", request->device_id);
        esp_http_client_set_post_field(client, "", 0);
    } else {
        snprintf(session, sizeof(session), "%" PRIu32, request->session_id);
        snprintf(index, sizeof(index), "%u", (unsigned)request->index);
        snprintf(duration, sizeof(duration), "%" PRIu32, request->duration_ms);
        snprintf(last, sizeof(last), "%d", request->is_last ? 1 : 0);
        esp_http_client_set_header(client, "X-Device-Id", request->device_id);
        esp_http_client_set_header(client, "X-Session-Id", session);
        esp_http_client_set_header(client, "X-Index", index);
        esp_http_client_set_header(client, "X-Duration-Ms", duration);
        esp_http_client_set_header(client, "X-Sha256", request->sha256);
        esp_http_client_set_header(client, "X-Is-Last", last);
        if (request->recorded_at[0] != '\0') {
            esp_http_client_set_header(client, "X-Recorded-At", request->recorded_at);
        }
        esp_http_client_set_post_field(client, (const char *)(body ? body : (const uint8_t *)""), (int)len);
    }
    started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    {
        int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        *status_out = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "post %s failed: %s bytes=%u ms=%" PRId64, request->path, esp_err_to_name(err),
                     (unsigned)len, elapsed_ms);
        } else {
            ESP_LOGI(TAG, "post %s ok status=%d bytes=%u ms=%" PRId64, request->path, *status_out, (unsigned)len,
                     elapsed_ms);
        }
    }
    esp_http_client_cleanup(client);
    return err == ESP_OK ? 0 : -1;
}

/* The recordings tree is shared with the UI and the recorder, so the scan
 * runs under the store lock. */
static int collect_sessions(const char *root, uint32_t *ids, size_t cap, size_t *out_count)
{
    DIR *dir;
    size_t count = 0;
    if (recorder_store_lock_acquire(RECORDER_UPLOAD_LOCK_TIMEOUT_MS) != 0) {
        *out_count = 0;
        return -1;
    }
    dir = opendir(root);
    if (!dir) {
        recorder_store_lock_release();
        *out_count = 0;
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long value;
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        value = strtoul(ent->d_name, &end, 10);
        if (!end || *end != '\0' || count >= cap) {
            continue;
        }
        ids[count++] = (uint32_t)value;
    }
    closedir(dir);
    recorder_store_lock_release();
    for (size_t i = 1; i < count; i++) {
        uint32_t value = ids[i];
        size_t j = i;
        while (j > 0 && ids[j - 1] > value) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = value;
    }
    *out_count = count;
    return 0;
}

static int load_closed_slices(recorder_store_t *store, uint32_t session_id, recorder_slice_t *slices, size_t cap,
                              size_t *out_count)
{
    size_t count = 0;
    for (uint16_t index = 0; index < RECORDER_SESSION_MAX_SLICES && count < cap; index++) {
        int is_last = recorder_store_is_last(store, session_id, index);
        uint32_t duration_ms = 0;
        if (is_last < 0) {
            break;
        }
        if (recorder_store_duration_ms(store, session_id, index, &duration_ms) != 0) {
            /* One damaged slice must not block every other session.
             *
             * recorder_store_inventory() reads the same meta files and
             * treats a slice it cannot read as the end of that session, so
             * the inventory still counts the sessions that are intact. The
             * upload used to fail outright here, which meant a single
             * damaged recording stopped the whole inbox from being
             * uploaded. Stop reading this session instead and carry on with
             * the next one. The damaged slice stays unacknowledged, so it is
             * retried later. */
            ESP_LOGE(TAG, "load: session=%" PRIu32 " index=%u has no readable duration; skipping the session",
                     session_id, (unsigned)index);
            break;
        }
        slices[count].session_id = session_id;
        slices[count].index = index;
        slices[count].duration_ms = duration_ms;
        slices[count].closed = true;
        slices[count].is_last = is_last == 1;
        slices[count].recorded_at[0] = '\0';
        (void)recorder_store_recorded_at(store, session_id, slices[count].recorded_at, sizeof(slices[count].recorded_at));
        count++;
        if (is_last == 1) {
            break;
        }
    }
    *out_count = count;
    return 0;
}

/* Read the slice into memory under the store lock, then post it without the
 * lock. The lock must never be held across the HTTP request: the request can
 * block for the whole timeout and the UI shares this tree. */
static int send_slice_body(recorder_store_t *store, const char *root, const recorder_upload_request_t *request,
                           int *status_out)
{
    char path[512];
    struct stat st;
    uint8_t *body = NULL;
    size_t len = 0;
    int posted;

    snprintf(path, sizeof(path), "%s/%" PRIu32 "/%u.ogg", root, request->session_id, (unsigned)request->index);
    if (recorder_store_lock_acquire(RECORDER_UPLOAD_LOCK_TIMEOUT_MS) != 0) {
        return -1;
    }
    if (stat(path, &st) != 0 || st.st_size < 0) {
        recorder_store_lock_release();
        return -1;
    }
    body = heap_caps_malloc((size_t)st.st_size + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc((size_t)st.st_size + 1u);
    }
    if (!body) {
        recorder_store_lock_release();
        return -1;
    }
    if (recorder_store_get(store, request->session_id, request->index, body, (size_t)st.st_size + 1u, &len) != 0) {
        free(body);
        recorder_store_lock_release();
        return -1;
    }
    /* The body is in memory now. The lock goes back before the request. */
    recorder_store_lock_release();
    posted = post_request(request, body, len, status_out);
    free(body);
    return posted;
}

/* Fail the upload with a readable reason. Every exit of the upload path
 * goes through here, so a silent failure can never reach the user. */
static int uplink_fail(const char *reason)
{
    s_failure = reason;
    ESP_LOGE(TAG, "upload failed: %s", reason);
    return -1;
}

/* The certificate bundle validates the validity window of the server
 * certificate against the system clock. Without a wall clock the handshake
 * fails with a TLS error that says nothing about the real problem.
 *
 * The upload runs on its own worker task, so it can wait for the clock
 * without freezing the UI. The board normally syncs a few seconds after it
 * joins the network, so the wait is short in practice. It is bounded: a
 * board that never syncs has to give up rather than sit on the task. */
#define UPLINK_CLOCK_WAIT_MS 60000
#define UPLINK_CLOCK_POLL_MS 500

static int uplink_clock_ready(void)
{
    uint32_t waited_ms = 0;
    recorder_upload_endpoint_t endpoint;

    /* The certificate bundle checks the validity window of the server
     * certificate against the system clock, so a device without a wall clock
     * cannot do the handshake.
     *
     * A plain HTTP link has no handshake and no certificate, so it does not
     * need the clock. That keeps the board usable on a network without a
     * route to the internet, where SNTP never answers and the inbox on the
     * developer's own machine is the only server there is. */
    if (recorder_upload_endpoint("/health", &endpoint) == 0 && endpoint.plain_http) {
        return 0;
    }
    while (!app_clock_valid()) {
        if (waited_ms >= UPLINK_CLOCK_WAIT_MS) {
            app_clock_log("upload gave up waiting for the clock");
            return uplink_fail("Clock not set");
        }
        vTaskDelay(pdMS_TO_TICKS(UPLINK_CLOCK_POLL_MS));
        waited_ms += UPLINK_CLOCK_POLL_MS;
    }
    return 0;
}

/* True when the recording is one of the ones the user picked.
 *
 * The list is the selection in the recordings panel. A recording that is not
 * on it is left alone, so an upload of one recording does not send the
 * others as well. */
static bool session_in_list(const uint32_t *ids, size_t count, uint32_t session_id)
{
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == session_id) {
            return true;
        }
    }
    return false;
}

int recorder_uplink_send_pending(const char *root, const char *device_id, const uint32_t *only, size_t only_count)
{
    uint32_t sessions[64];
    size_t session_count = 0;
    recorder_upload_cursor_t cursor;
    recorder_store_t *store;
    int sent = 0;

    s_failure = NULL;
    s_note = NULL;
    if (!root || !device_id || !device_id[0]) {
        return uplink_fail("Inbox address missing");
    }
    if (uplink_clock_ready() != 0) {
        return -1;
    }
    store = recorder_store_open(root);
    if (!store) {
        return uplink_fail("Storage unavailable");
    }
    if (collect_sessions(root, sessions, 64, &session_count) != 0) {
        recorder_store_close(store);
        return uplink_fail("Storage unavailable");
    }
    ESP_LOGI(TAG, "upload: %u recording(s) on the device, %u picked", (unsigned)session_count,
             (unsigned)only_count);
    recorder_upload_cursor_init(&cursor);
    for (size_t session_i = 0; session_i < session_count; session_i++) {
        recorder_slice_t slices[RECORDER_SESSION_MAX_SLICES];
        size_t slice_count = 0;
        cursor.next_index = 0;
        cursor.in_flight = false;
        /* A recording the user did not pick is not sent. */
        if (only && !session_in_list(only, only_count, sessions[session_i])) {
            continue;
        }
        ESP_LOGI(TAG, "upload: session=%" PRIu32 " of %u", sessions[session_i], (unsigned)session_count);
        /* The slice list is read in one transaction, so the tree cannot move
         * under it while the upload runs. */
        if (recorder_store_lock_acquire(RECORDER_UPLOAD_LOCK_TIMEOUT_MS) != 0) {
            recorder_store_close(store);
            return uplink_fail("Storage unavailable");
        }
        if (load_closed_slices(store, sessions[session_i], slices, RECORDER_SESSION_MAX_SLICES, &slice_count) != 0) {
            recorder_store_lock_release();
            recorder_store_close(store);
            return uplink_fail("Cannot read recording");
        }
        while (cursor.next_index < slice_count &&
                recorder_store_is_uploaded(store, sessions[session_i], cursor.next_index) == 1) {
            cursor.next_index++;
        }
        recorder_store_lock_release();
        if (slice_count == 0) {
            continue;
        }
        if (cursor.next_index >= slice_count) {
            /* Every slice of this recording was uploaded before, so there is
             * nothing to send for it. */
            s_note = "Already on the Mac";
            continue;
        }
        while (1) {
            char sha[65] = {0};
            recorder_upload_request_t request;
            int status = 0;
            const char *sha_arg = NULL;
            if (cursor.health_ok) {
                /* The checksum is read under the lock and the lock is dropped
                 * again before the request goes out. */
                if (recorder_store_lock_acquire(RECORDER_UPLOAD_LOCK_TIMEOUT_MS) != 0) {
                    recorder_store_close(store);
                    return uplink_fail("Storage unavailable");
                }
                if (recorder_store_sha256(store, sessions[session_i], cursor.next_index, sha) != 0) {
                    /* The session has no slice at this index any more, so it
                     * is finished. This is the end of the session, not a
                     * failure: the slices before it were uploaded and
                     * acknowledged. */
                    recorder_store_lock_release();
                    ESP_LOGI(TAG, "session %" PRIu32 " is fully uploaded", sessions[session_i]);
                    break;
                }
                recorder_store_lock_release();
                sha_arg = sha;
            }
            if (recorder_upload_prepare(&cursor, device_id, slices, slice_count, sha_arg, &request) != 0) {
                /* There is no slice left for this session. That is the end of
                 * the session, not a failure, so the next one starts. */
                ESP_LOGI(TAG, "session %" PRIu32 " is fully uploaded", sessions[session_i]);
                break;
            }
            if (request.kind == RECORDER_UPLOAD_SLICE) {
                if (send_slice_body(store, root, &request, &status) != 0) {
                    recorder_upload_result(&cursor, status);
                    recorder_store_close(store);
                    return uplink_fail("Inbox unavailable");
                }
            } else if (post_request(&request, NULL, 0, &status) != 0) {
                recorder_upload_result(&cursor, status);
                recorder_store_close(store);
                return uplink_fail("Inbox unavailable");
            }
            recorder_upload_result(&cursor, status);
            if (status != 200) {
                recorder_store_close(store);
                return uplink_fail(status == 400 ? "Checksum rejected" : "Upload failed");
            }
            if (request.kind == RECORDER_UPLOAD_SLICE) {
                if (recorder_store_mark_uploaded(store, request.session_id, request.index) != 0) {
                    recorder_store_close(store);
                    return uplink_fail("Cannot save upload state");
                }
                sent++;
                ESP_LOGI(TAG, "uploaded session=%" PRIu32 " index=%u", request.session_id, (unsigned)request.index);
            }
        }
    }
    recorder_store_close(store);
    ESP_LOGI(TAG, "upload: sent %d slice(s)", sent);
    return sent;
}

const char *recorder_uplink_failure(void)
{
    return s_failure ? s_failure : "Upload failed";
}

const char *recorder_uplink_note(void)
{
    return s_note;
}

void recorder_uplink_device_id(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    if (!out || cap < 13) {
        return;
    }
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
