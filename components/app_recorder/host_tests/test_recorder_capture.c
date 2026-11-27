#include "recorder_capture.h"
#include "recorder_session.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void samples_past_the_slice_limit_close_full_slices(void)
{
    const size_t samples = 25000u * (RECORDER_CAPTURE_HZ / 1000u);
    int16_t *pcm = calloc(samples, sizeof(*pcm));
    recorder_session_t *session = recorder_session_create(1, 10000);

    expect(pcm != NULL && session != NULL, "fixtures");
    expect(recorder_capture_push(session, pcm, samples) == 0, "push 25 seconds");
    expect(recorder_session_slice_count(session) == 3, "two closed slices plus remainder");

    const recorder_slice_t *first = recorder_session_slice(session, 0);
    const recorder_slice_t *second = recorder_session_slice(session, 1);
    const recorder_slice_t *tail = recorder_session_slice(session, 2);
    expect(first && first->closed && !first->is_last && first->duration_ms == 10000, "first slice is full");
    expect(second && second->closed && !second->is_last && second->duration_ms == 10000, "second slice is full");
    expect(tail && !tail->closed && !tail->is_last && tail->duration_ms == 5000, "five seconds stay open");

    free(pcm);
    recorder_session_destroy(session);
}

static void partial_milliseconds_are_kept(void)
{
    recorder_session_t *session = recorder_session_create(2, 10000);
    int16_t pcm[8] = {0};

    expect(recorder_capture_push(session, pcm, 8) == 0, "first half-millisecond");
    expect(recorder_session_slice_count(session) == 0, "half a millisecond does not open a slice");
    expect(recorder_capture_push(session, pcm, 8) == 0, "second half-millisecond");

    const recorder_slice_t *slice = recorder_session_slice(session, 0);
    expect(recorder_session_slice_count(session) == 1, "a full millisecond opens the slice");
    expect(slice && !slice->closed && slice->duration_ms == 1, "eight plus eight samples are one millisecond");
    recorder_session_destroy(session);
}

int main(void)
{
    samples_past_the_slice_limit_close_full_slices();
    partial_milliseconds_are_kept();
    return g_failed ? 1 : 0;
}
