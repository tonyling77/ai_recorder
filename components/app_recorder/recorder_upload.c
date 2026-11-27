#include "recorder_upload_config.h"

#include "recorder_inbox.h"
#include "recorder_upload.h"

#include <stdio.h>
#include <string.h>

int recorder_upload_next(const recorder_slice_t *slices, size_t count, uint16_t next_index, recorder_slice_t *out)
{
    if (!slices || !out) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (slices[i].index != next_index) {
            continue;
        }
        if (!slices[i].closed) {
            return -1;
        }
        *out = slices[i];
        return 0;
    }
    return -1;
}

void recorder_upload_cursor_init(recorder_upload_cursor_t *cursor)
{
    if (!cursor) {
        return;
    }
    cursor->next_index = 0;
    cursor->health_ok = false;
    cursor->in_flight = false;
}

static void copy_text(char *dest, size_t cap, const char *src)
{
    if (!dest || cap == 0) {
        return;
    }
    dest[0] = '\0';
    if (!src) {
        return;
    }
    strncpy(dest, src, cap - 1);
    dest[cap - 1] = '\0';
}

int recorder_upload_prepare(recorder_upload_cursor_t *cursor, const char *device_id,
                            const recorder_slice_t *slices, size_t count, const char *sha256,
                            recorder_upload_request_t *out)
{
    recorder_slice_t slice;

    if (!cursor || !device_id || !device_id[0] || !out || cursor->in_flight) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    copy_text(out->device_id, sizeof(out->device_id), device_id);
    if (!cursor->health_ok) {
        out->kind = RECORDER_UPLOAD_HEALTH;
        copy_text(out->path, sizeof(out->path), "/health");
        cursor->in_flight = true;
        return 0;
    }
    if (recorder_upload_next(slices, count, cursor->next_index, &slice) != 0 || !sha256 || strlen(sha256) != 64) {
        return -1;
    }
    out->kind = RECORDER_UPLOAD_SLICE;
    copy_text(out->path, sizeof(out->path), "/v1/slices");
    out->session_id = slice.session_id;
    out->index = slice.index;
    out->duration_ms = slice.duration_ms;
    out->is_last = slice.is_last;
    out->has_slice = true;
    snprintf(out->recorded_at, sizeof(out->recorded_at), "%s", slice.recorded_at);
    copy_text(out->sha256, sizeof(out->sha256), sha256);
    cursor->in_flight = true;
    return 0;
}

int recorder_upload_endpoint(const char *path, recorder_upload_endpoint_t *out)
{
    char learned[RECORDER_INBOX_ORIGIN_MAX];
    const char *origin;
    const char *token = RECORDER_UPLOAD_TOKEN;
    int written;

    if (!path || path[0] != '/' || strchr(path, '?') || strstr(path, "token=") || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    /* The origin that is learned at runtime comes first: it is the one the
     * developer pointed the board at, and it survives a change of the
     * address of the machine. The build-time origin is the fallback, so a
     * board that was built for a private inbox keeps working. */
    if (recorder_inbox_get(learned, sizeof(learned), NULL, 0) != 0) {
        learned[0] = '\0';
    }
    origin = learned[0] ? learned : RECORDER_UPLOAD_ORIGIN;
    if (!origin || !origin[0]) {
        return -1;
    }
    written = snprintf(out->url, sizeof(out->url), "%s%s", origin, path);
    if (written < 0 || (size_t)written >= sizeof(out->url)) {
        return -1;
    }
    /* The token travels in a header. The local inbox on the developer's own
     * machine does not check it, so it may be empty. When it is there, it
     * must not end up in the URL. */
    if (token && token[0]) {
        if (strstr(out->url, token) || strstr(out->url, "token=")) {
            return -1;
        }
    }
    out->timeout_ms = RECORDER_UPLOAD_HEALTH_TIMEOUT_MS;
    out->slice_timeout_ms = RECORDER_UPLOAD_SLICE_TIMEOUT_MS;
    /* The scheme of the origin decides whether the link is encrypted.
     *
     * The https origin is the tunnel to a private inbox. The http origin is
     * the inbox on the developer's own machine, on their own network, where
     * plain text is what the setup asks for. Anything that is not https is
     * plain: the build only accepts these two schemes, so a link never
     * loses its certificate by accident. */
    if (strncmp(origin, "https://", 8) == 0) {
        out->plain_http = 0;
        out->cert_bundle = 1;
    } else {
        out->plain_http = 1;
        out->cert_bundle = 0;
    }
    out->insecure = 0;
    out->token_in_url = 0;
    memcpy(out->token_header, "X-Auth-Token", sizeof("X-Auth-Token"));
    return 0;
}

void recorder_upload_result(recorder_upload_cursor_t *cursor, int http_status)
{
    if (!cursor || !cursor->in_flight) {
        return;
    }
    cursor->in_flight = false;
    if (http_status != 200) {
        return;
    }
    if (!cursor->health_ok) {
        cursor->health_ok = true;
        return;
    }
    if (cursor->next_index < UINT16_MAX) {
        cursor->next_index++;
    }
}
