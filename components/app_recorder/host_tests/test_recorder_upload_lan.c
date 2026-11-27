/* The private channel over plain HTTP, to the inbox on the developer's own
 * machine.
 *
 * The binary is built with an http:// origin, so this file checks the shape
 * of a plain link: no certificate bundle, no insecure flag, the token in the
 * header, and the same timeouts as the tunnel link. The https behaviour is
 * checked by test_recorder_upload.c, which is built with an https origin.
 */

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

static void the_lan_origin_has_no_certificate(void)
{
    recorder_upload_endpoint_t endpoint;

    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "health endpoint");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0, "health uses the injected origin");
    expect(strncmp(endpoint.url, "http://", 7) == 0, "the origin stays on http");
    expect(endpoint.plain_http == 1, "the link is plain");
    expect(endpoint.cert_bundle == 0, "no certificate bundle on a link without a certificate");
    expect(endpoint.insecure == 0, "the link is not marked insecure either");
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

    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the build-time origin is used");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0,
           "the url is the build-time origin");

    /* A learned plain origin replaces it, and it stays plain. */
    expect(recorder_inbox_set("http://other.local:9000", "other") == 0, "an origin is learned");
    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the learned origin is used");
    expect(strcmp(endpoint.url, "http://other.local:9000/health") == 0,
           "the url is the learned origin");
    expect(endpoint.plain_http == 1 && endpoint.cert_bundle == 0,
           "the learned plain link has no certificate");

    recorder_inbox_clear();
    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "the build-time origin is used again");
    expect(strcmp(endpoint.url, RECORDER_UPLOAD_ORIGIN "/health") == 0,
           "the url is the build-time origin again");
}

static void the_timeouts_do_not_change(void)
{
    recorder_upload_endpoint_t endpoint;
    const int bytes = 2200000;
    const int bits_per_sec = 100000;
    const int need_ms = (int)(((long long)bytes * 8 * 1000) / bits_per_sec);

    expect(recorder_upload_endpoint("/health", &endpoint) == 0, "endpoint for budget");
    expect(endpoint.timeout_ms == 20000, "a dead link is reported within 20s");
    expect(endpoint.timeout_ms < need_ms, "the health check does not wait for a slow slice");
    expect(endpoint.slice_timeout_ms >= need_ms && endpoint.slice_timeout_ms == 300000,
           "300s covers one 2.2MB slice at 100 kbit/s");
    expect(endpoint.slice_timeout_ms > endpoint.timeout_ms, "a slice gets more time than the health check");
}

static void the_upload_still_walks_the_slices_one_at_a_time(void)
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
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 3, sha, &request) == 0 &&
               request.kind == RECORDER_UPLOAD_HEALTH,
           "health is prepared first");
    recorder_upload_result(&cursor, 200);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 3, sha, &request) == 0 &&
               request.kind == RECORDER_UPLOAD_SLICE && request.index == 0,
           "a slice follows health");
    recorder_upload_result(&cursor, 200);
    expect(recorder_upload_prepare(&cursor, "board-1", slices, 3, sha, &request) == 0 && request.index == 1 &&
               request.is_last,
           "the last closed slice follows");
}

int main(void)
{
    the_lan_origin_has_no_certificate();
    the_token_travels_in_the_header_only();
    a_learned_origin_comes_first();
    the_timeouts_do_not_change();
    the_upload_still_walks_the_slices_one_at_a_time();
    return g_failed ? 1 : 0;
}
