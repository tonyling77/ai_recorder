#include "recorder_encoder.h"
#include "recorder_frames.h"
#include "recorder_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void a_partial_frame_is_not_encoded(void)
{
    recorder_session_t *session = recorder_session_create(3, 10000);
    recorder_encoder_t *encoder = recorder_encoder_create();
    recorder_ogg_t *ogg = recorder_ogg_create();
    recorder_frames_t *frames = recorder_frames_create(encoder, ogg);
    int16_t pcm[RECORDER_FRAME_SAMPLES];
    const uint8_t *ogg_bytes = NULL;
    size_t ogg_len = 0;
    const recorder_slice_t *slice = NULL;

    for (size_t i = 0; i < RECORDER_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }

    expect(session && encoder && ogg && frames, "fixtures");
    expect(recorder_frames_push(frames, session, pcm, 100) == 0, "microphone returns a short read");
    expect(recorder_ogg_bytes(ogg, &ogg_bytes, &ogg_len) == 0 && ogg_len == 0, "a partial frame is not encoded");
    slice = recorder_session_slice(session, 0);
    expect(slice && slice->duration_ms == 6, "those samples still enter the session");

    expect(recorder_frames_push(frames, session, pcm + 100, RECORDER_FRAME_SAMPLES - 100) == 0,
           "the rest of the 20 ms arrives");
    expect(recorder_ogg_bytes(ogg, &ogg_bytes, &ogg_len) == 0 && ogg_len > 4 && memcmp(ogg_bytes, "OggS", 4) == 0,
           "the completed frame is wrapped");
    slice = recorder_session_slice(session, 0);
    expect(slice && slice->duration_ms == RECORDER_FRAME_MS, "the session shows the whole frame");

    recorder_frames_destroy(frames);
    recorder_ogg_destroy(ogg);
    recorder_encoder_destroy(encoder);
    recorder_session_destroy(session);
}

static int last_page_is_end(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    int end = 0;
    int pages = 0;

    if (!data) {
        return 0;
    }
    while (offset + 27 <= len && memcmp(data + offset, "OggS", 4) == 0) {
        uint8_t segments = data[offset + 26];
        size_t body = 0;
        if (offset + 27u + segments > len) {
            return 0;
        }
        for (uint8_t i = 0; i < segments; i++) {
            body += data[offset + 27u + i];
        }
        if (offset + 27u + segments + body > len) {
            return 0;
        }
        end = (data[offset + 5] & 0x04) != 0;
        pages++;
        offset += 27u + segments + body;
    }
    return pages > 0 && end && offset == len;
}

static void stopping_wraps_the_short_tail(void)
{
    recorder_session_t *session = recorder_session_create(4, 10000);
    recorder_encoder_t *encoder = recorder_encoder_create();
    recorder_ogg_t *ogg = recorder_ogg_create();
    recorder_frames_t *frames = recorder_frames_create(encoder, ogg);
    int16_t pcm[100];
    const uint8_t *ogg_bytes = NULL;
    size_t ogg_len = 0;
    const recorder_slice_t *slice = NULL;

    for (size_t i = 0; i < 100; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }
    expect(session && encoder && ogg && frames, "stop fixtures");
    expect(recorder_frames_push(frames, session, pcm, 100) == 0, "short tail arrives");
    expect(recorder_frames_finish(frames, session) == 0, "stop");
    expect(recorder_ogg_bytes(ogg, &ogg_bytes, &ogg_len) == 0 && last_page_is_end(ogg_bytes, ogg_len),
           "the short tail is the end of the ogg");
    slice = recorder_session_slice(session, 0);
    expect(slice && slice->closed && slice->is_last && slice->duration_ms == 6, "the short take is closed");

    recorder_frames_destroy(frames);
    recorder_ogg_destroy(ogg);
    recorder_encoder_destroy(encoder);
    recorder_session_destroy(session);
}

static int page_granule(const uint8_t *data, size_t len, int page_index, uint64_t *granule, int *page_count)
{
    size_t offset = 0;
    int pages = 0;

    if (!data || !granule || !page_count) {
        return 0;
    }
    *granule = 0;
    while (offset + 27 <= len && memcmp(data + offset, "OggS", 4) == 0) {
        uint8_t segments = data[offset + 26];
        size_t body = 0;
        uint64_t value = 0;
        if (offset + 27u + segments > len) {
            return 0;
        }
        for (uint8_t i = 0; i < segments; i++) {
            body += data[offset + 27u + i];
        }
        if (offset + 27u + segments + body > len) {
            return 0;
        }
        for (int i = 0; i < 8; i++) {
            value |= (uint64_t)data[offset + 6u + i] << (8 * i);
        }
        if (pages == page_index) {
            *granule = value;
        }
        pages++;
        offset += 27u + segments + body;
    }
    *page_count = pages;
    return offset == len;
}

static void two_seconds_stay_in_one_ogg(void)
{
    const size_t frames = 100;
    const size_t samples = frames * RECORDER_FRAME_SAMPLES;
    recorder_session_t *session = recorder_session_create(5, 10000);
    recorder_encoder_t *encoder = recorder_encoder_create();
    recorder_ogg_t *ogg = recorder_ogg_create();
    recorder_frames_t *frames_enc = recorder_frames_create(encoder, ogg);
    int16_t *pcm = calloc(samples, sizeof(*pcm));
    const uint8_t *ogg_bytes = NULL;
    size_t ogg_len = 0;
    uint64_t granule = 0;
    int pages = 0;

    expect(session && encoder && ogg && frames_enc && pcm, "two second fixtures");
    expect(recorder_frames_push(frames_enc, session, pcm, samples) == 0, "two seconds encode");
    expect(recorder_ogg_bytes(ogg, &ogg_bytes, &ogg_len) == 0 &&
               page_granule(ogg_bytes, ogg_len, 3, &granule, &pages),
           "pages parse");
    expect(pages == 102, "headers plus one page per frame");
    expect(granule == 1920, "the second audio page is 40 ms");

    free(pcm);
    recorder_frames_destroy(frames_enc);
    recorder_ogg_destroy(ogg);
    recorder_encoder_destroy(encoder);
    recorder_session_destroy(session);
}

static void stopping_on_a_frame_boundary_ends_the_ogg(void)
{
    recorder_session_t *session = recorder_session_create(6, 10000);
    recorder_encoder_t *encoder = recorder_encoder_create();
    recorder_ogg_t *ogg = recorder_ogg_create();
    recorder_frames_t *frames = recorder_frames_create(encoder, ogg);
    int16_t pcm[RECORDER_FRAME_SAMPLES] = {0};
    const uint8_t *ogg_bytes = NULL;
    size_t ogg_len = 0;

    expect(session && encoder && ogg && frames, "boundary fixtures");
    expect(recorder_frames_push(frames, session, pcm, RECORDER_FRAME_SAMPLES) == 0, "one full frame");
    expect(recorder_frames_finish(frames, session) == 0, "stop on the boundary");
    expect(recorder_ogg_bytes(ogg, &ogg_bytes, &ogg_len) == 0 && last_page_is_end(ogg_bytes, ogg_len),
           "the last full frame ends the ogg");

    recorder_frames_destroy(frames);
    recorder_ogg_destroy(ogg);
    recorder_encoder_destroy(encoder);
    recorder_session_destroy(session);
}

int main(void)
{
    a_partial_frame_is_not_encoded();
    stopping_wraps_the_short_tail();
    two_seconds_stay_in_one_ogg();
    stopping_on_a_frame_boundary_ends_the_ogg();
    return g_failed ? 1 : 0;
}
