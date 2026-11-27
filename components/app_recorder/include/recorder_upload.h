#pragma once

#include "recorder_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int recorder_upload_next(const recorder_slice_t *slices, size_t count, uint16_t next_index, recorder_slice_t *out);

typedef enum {
    RECORDER_UPLOAD_HEALTH = 1,
    RECORDER_UPLOAD_SLICE = 2,
} recorder_upload_kind_t;

typedef struct {
    uint16_t next_index;
    bool health_ok;
    bool in_flight;
} recorder_upload_cursor_t;

typedef struct {
    recorder_upload_kind_t kind;
    char path[32];
    char device_id[48];
    uint32_t session_id;
    uint16_t index;
    uint32_t duration_ms;
    char sha256[65];
    bool is_last;
    bool has_slice;
    char recorded_at[16];
} recorder_upload_request_t;

void recorder_upload_cursor_init(recorder_upload_cursor_t *cursor);
int recorder_upload_prepare(recorder_upload_cursor_t *cursor, const char *device_id,
                            const recorder_slice_t *slices, size_t count, const char *sha256,
                            recorder_upload_request_t *out);
void recorder_upload_result(recorder_upload_cursor_t *cursor, int http_status);

/* RECORDER_UPLOAD_ORIGIN is injected at configure time from UPLOAD_ORIGIN.
 * Do not hard-code a production host in this header.
 *
 * It is the fallback: an origin that was learned at runtime comes first, so
 * a board that was built for anybody works with the inbox of the developer
 * who is holding it. */
/* Two timeouts, because the two requests have very different shapes.
 *
 * The health check is a few bytes. It is the request that tells the user
 * whether the link is alive at all, so it must fail fast: a dead link used
 * to hold the screen on "Uploading" for five minutes with nothing to show
 * for it.
 *
 * A slice POST carries up to RECORDER_SLICE_LIMIT_MS of Opus audio, about
 * 2.2 MB. Over a slow WAN at 100 kbit/s that needs roughly 176 seconds, so
 * the slice POST keeps the long budget. Cutting it to the health timeout
 * would break uploads on exactly the links that need them most. */
#define RECORDER_UPLOAD_HEALTH_TIMEOUT_MS 20000
#define RECORDER_UPLOAD_SLICE_TIMEOUT_MS 300000

/* The timeout a request gets when nothing says otherwise. */
#define RECORDER_UPLOAD_TIMEOUT_MS RECORDER_UPLOAD_HEALTH_TIMEOUT_MS

typedef struct {
    char url[160];
    int timeout_ms;
    int slice_timeout_ms;
    int cert_bundle;
    int insecure;
    /* The origin is http://, so the link carries no TLS.
     *
     * A developer runs the inbox on the machine they already work on, and
     * the recording travels in plain text inside their own network. There
     * is no certificate to trust and no handshake, so the certificate
     * bundle is not attached and the device does not need a wall clock.
     * An https origin keeps the bundle and the handshake. */
    int plain_http;
    char token_header[16];
    int token_in_url;
} recorder_upload_endpoint_t;

int recorder_upload_endpoint(const char *path, recorder_upload_endpoint_t *out);
