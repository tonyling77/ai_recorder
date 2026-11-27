#include "recorder_stamp.h"
#include "recorder_inbox.h"
#include "recorder_upload.h"
#include "recorder_upload_config.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void the_next_upload_is_the_lowest_closed_slice(void)
{
    recorder_slice_t slices[3] = {
        {.session_id = 1, .index = 0, .duration_ms = 10000, .closed = true, .is_last = false},
        {.session_id = 1, .index = 1, .duration_ms = 10000, .closed = true, .is_last = false},
        {.session_id = 1, .index = 2, .duration_ms = 4000, .closed = false, .is_last = false},
    };
    recorder_slice_t next = {0};

    expect(recorder_upload_next(slices, 3, 0, &next) == 0 && next.index == 0, "send the first closed slice");
    expect(recorder_upload_next(slices, 3, 1, &next) == 0 && next.index == 1, "then the second");
    expect(recorder_upload_next(slices, 3, 2, &next) != 0, "the open tail stays on the device");

    slices[1].index = 2;
    expect(recorder_upload_next(slices, 3, 1, &next) != 0, "a missing index blocks the later slice");
}

static void health_then_one_closed_slice_at_a_time(void)
{
    static const char *sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    recorder_slice_t slices[3] = {
        {.session_id = 4, .index = 0, .duration_ms = 20, .closed = true, .is_last = false,
         .recorded_at = "20261111-1126"},
        {.session_id = 4, .index = 1, .duration_ms = 20, .closed = true, .is_last = true},
        {.session_id = 4, .index = 2, .duration_ms = 20, .closed = false, .is_last = false},
    };
    recorder_upload_cursor_t cursor;
    recorder_upload_request_t request;

    recorder_upload_cursor_init(&cursor);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 2, sha, &request) == 0, "health is prepared");
    expect(request.kind == RECORDER_UPLOAD_HEALTH && strcmp(request.path, "/health") == 0, "first call checks the inbox");
    expect(strcmp(request.device_id, "board-1") == 0 && !request.has_slice, "no audio before the inbox answers");
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 2, sha, &request) != 0, "one request is in flight");

    recorder_upload_result(&cursor, 400);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 2, sha, &request) == 0 &&
               request.kind == RECORDER_UPLOAD_HEALTH,
           "a rejected health check is repeated");
    recorder_upload_result(&cursor, 200);

    expect(recorder_upload_prepare(&cursor, "board-1", slices, 2, sha, &request) == 0, "a slice follows health");
    expect(request.kind == RECORDER_UPLOAD_SLICE && strcmp(request.path, "/v1/slices") == 0, "slice path");
    expect(request.session_id == 4 && request.index == 0 && !request.is_last && request.duration_ms == 20,
           "the lowest closed slice is first");
    expect(strcmp(request.sha256, sha) == 0, "the checksum travels with the slice");
    expect(strcmp(request.recorded_at, "20261111-1126") == 0, "the slice carries its start time");
    recorder_upload_result(&cursor, 400);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 2, sha, &request) == 0 && request.index == 0,
           "a checksum mismatch stays pending");
    recorder_upload_result(&cursor, 200);

    expect(recorder_upload_prepare(&cursor, "board-1", slices, 3, sha, &request) == 0 && request.index == 1 &&
               request.is_last,
           "the last closed slice follows");
    recorder_upload_result(&cursor, 200);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 3, sha, &request) != 0, "the open slice stays on the device");
}

static void injected_origin_is_joined_to_the_path(void)
{
    recorder_upload_endpoint_t endpoint;

    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "health endpoint");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0,
           "health uses the injected origin");
    expect(recorder_upload_endpoint("/v1/slices", &endpoint) == 0, "slice endpoint");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/v1/slices") == 0,
           "slices use the injected origin");
    expect(strncmp(endpoint.url, "https://", 8) == 0, "the origin stays on https");
}

static void the_token_travels_in_the_header_only(void)
{
    recorder_upload_endpoint_t endpoint;

    expect(recorder_upload_endpoint("/v1/slices", &endpoint) == 0, "a slice endpoint is accepted");
    expect(strcmp(endpoint.token_header, "X-Auth-Token") == 0, "the header name is X-Auth-Token");
    expect(endpoint.token_in_url == 0, "the token is not a query parameter");
    expect(strstr(endpoint.url, "token=") == NULL, "the url does not carry a token");
}

static void a_learned_origin_comes_first(void)
{
    recorder_upload_endpoint_t endpoint;

    /* Nothing is learned yet, so the build-time origin is used. */
    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the build-time origin is used");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0,
           "the url is the build-time origin");

    /* A learned origin comes first. It is a name, so the address of the
     * machine may change without the device losing the inbox. */
    expect(recorder_inbox_set("http://laptop.local:8765", "laptop") == 0, "an origin is learned");
    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the learned origin is used");
    expect(strcmp(endpoint.url, "http://laptop.local:8765/health") == 0,
           "the url is the learned origin");
    expect(endpoint.plain_http == 1, "the learned plain link has no certificate");

    /* An origin that is not a base URL is refused. */
    expect(recorder_inbox_set("ftp://laptop.local", "laptop") != 0, "a scheme that is not http is refused");
    expect(recorder_inbox_set("http://laptop.local:8765/", "laptop") != 0, "a trailing slash is refused");
    expect(recorder_inbox_set("http://laptop.local:8765/notes", "laptop") != 0, "a path is refused");
    expect(recorder_inbox_set("", "laptop") != 0, "an empty origin is refused");
    expect(recorder_inbox_set(NULL, "laptop") != 0, "a missing origin is refused");

    recorder_inbox_clear();
    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the build-time origin is used again");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0,
           "the url is the build-time origin again");
}

static void wan_timeout_and_public_ca_stay_on(void)
{
    recorder_upload_endpoint_t endpoint;
    /* One slice carries up to RECORDER_SLICE_LIMIT_MS of Opus audio, about
     * 2.2 MB. Over a slow WAN at 100 kbit/s that needs roughly 176 seconds,
     * so the slice POST keeps a long budget. */
    const int bytes = 2200000;
    const int bits_per_sec = 100000;
    const int need_ms = (int)(((long long)bytes * 8 * 1000) / bits_per_sec);

    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "endpoint for budget");
    /* The health check is the request that tells the user whether the link
     * is alive. It must fail fast, so it gets the short timeout. */
    expect(endpoint.timeout_ms == 20000, "a dead link is reported within 20s");
    expect(endpoint.timeout_ms < need_ms, "the health check does not wait for a slow slice");
    /* The slice POST carries the megabytes, so it keeps the WAN budget. */
    expect(endpoint.slice_timeout_ms >= need_ms && endpoint.slice_timeout_ms == 300000,
           "300s covers one 2.2MB slice at 100 kbit/s");
    expect(endpoint.slice_timeout_ms > endpoint.timeout_ms, "a slice gets more time than the health check");
    expect(endpoint.cert_bundle == 1 && endpoint.insecure == 0, "public CA check stays on");
    expect(endpoint.plain_http == 0, "the tunnel link keeps its handshake");
}

static void a_recording_stamp_is_year_month_day_hour_minute(void)
{
    char stamp[16];

    expect(recorder_stamp_format(2026, 11, 11, 11, 26, stamp, sizeof(stamp)) == 0, "format the start time");
    expect(strcmp(stamp, "20261111-1126") == 0, "stamp is YYYYMMDD-HHMM");
    expect(recorder_stamp_format(1970, 1, 1, 0, 0, stamp, sizeof(stamp)) != 0, "an unsynced clock is not a stamp");
}

int main(void)
{
    a_recording_stamp_is_year_month_day_hour_minute();
    the_next_upload_is_the_lowest_closed_slice();
    health_then_one_closed_slice_at_a_time();
    injected_origin_is_joined_to_the_path();
    the_token_travels_in_the_header_only();
    a_learned_origin_comes_first();
    wan_timeout_and_public_ca_stay_on();
    return g_failed ? 1 : 0;
}
