#include "recorder_session.h"

#include <stdio.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void splits_audio_past_the_slice_limit(void)
{
    recorder_session_t *session = recorder_session_create(7, 10000);
    expect(session != NULL, "session created");
    expect(recorder_session_add_ms(session, 25000) == 0, "add past two limits");

    expect(recorder_session_slice_count(session) == 3, "two closed slices plus remainder");
    const recorder_slice_t *a = recorder_session_slice(session, 0);
    const recorder_slice_t *b = recorder_session_slice(session, 1);
    const recorder_slice_t *c = recorder_session_slice(session, 2);
    expect(a && a->session_id == 7 && a->index == 0 && a->duration_ms == 10000 && a->closed && !a->is_last,
           "first slice is a full closed piece");
    expect(b && b->session_id == 7 && b->index == 1 && b->duration_ms == 10000 && b->closed && !b->is_last,
           "second slice is a full closed piece");
    expect(c && c->session_id == 7 && c->index == 2 && c->duration_ms == 5000 && !c->closed && !c->is_last,
           "remainder stays open");
    recorder_session_destroy(session);
}

static void stop_closes_the_open_slice_as_last(void)
{
    recorder_session_t *session = recorder_session_create(7, 10000);
    expect(recorder_session_add_ms(session, 25000) == 0, "record before stop");
    expect(recorder_session_stop(session) == 0, "stop");

    expect(recorder_session_slice_count(session) == 3, "stop keeps the remainder");
    const recorder_slice_t *last = recorder_session_slice(session, 2);
    const recorder_slice_t *middle = recorder_session_slice(session, 1);
    expect(last && last->closed && last->is_last && last->duration_ms == 5000, "remainder is the last slice");
    expect(middle && middle->closed && !middle->is_last, "earlier slices stay unmarked");
    recorder_session_destroy(session);
}

static void only_closed_slices_are_uploadable(void)
{
    recorder_session_t *session = recorder_session_create(3, 10000);
    expect(recorder_session_add_ms(session, 25000) == 0, "record before upload check");
    expect(!recorder_slice_uploadable(recorder_session_slice(session, 2)), "open remainder stays on the device");
    expect(recorder_slice_uploadable(recorder_session_slice(session, 0)), "first closed slice can be sent");
    expect(recorder_slice_uploadable(recorder_session_slice(session, 1)), "second closed slice can be sent");
    expect(recorder_session_stop(session) == 0, "stop before sending the tail");
    expect(recorder_slice_uploadable(recorder_session_slice(session, 2)), "stopped remainder can be sent");
    expect(!recorder_slice_uploadable(NULL), "missing slice is not sent");
    recorder_session_destroy(session);
}

static void summary_waits_for_every_slice_and_the_last_mark(void)
{
    recorder_session_t *session = recorder_session_create(9, 10000);
    expect(recorder_session_add_ms(session, 25000) == 0, "record before join check");
    recorder_slice_t open_set[3];
    for (size_t i = 0; i < 3; i++) {
        open_set[i] = *recorder_session_slice(session, i);
    }
    expect(!recorder_slices_ready_for_summary(open_set, 3), "no summary while the tail is still open");

    expect(recorder_session_stop(session) == 0, "stop before join");
    recorder_slice_t full[3];
    for (size_t i = 0; i < 3; i++) {
        full[i] = *recorder_session_slice(session, i);
    }
    expect(recorder_slices_ready_for_summary(full, 3), "contiguous closed slices with is_last can be joined");

    recorder_slice_t gap[2] = {full[0], full[2]};
    expect(!recorder_slices_ready_for_summary(gap, 2), "a missing index blocks the summary");
    full[2].is_last = false;
    expect(!recorder_slices_ready_for_summary(full, 3), "missing is_last blocks the summary");
    expect(!recorder_slices_ready_for_summary(NULL, 0), "empty set is not a transcript");
    recorder_session_destroy(session);
}

static void stop_on_a_slice_boundary_drops_the_empty_tail(void)
{
    recorder_session_t *session = recorder_session_create(4, 10000);
    expect(recorder_session_add_ms(session, 20000) == 0, "record two exact slices");
    expect(recorder_session_stop(session) == 0, "stop on the boundary");
    expect(recorder_session_slice_count(session) == 2, "no empty tail");
    const recorder_slice_t *first = recorder_session_slice(session, 0);
    const recorder_slice_t *last = recorder_session_slice(session, 1);
    expect(first && first->duration_ms == 10000 && first->closed && !first->is_last, "first slice stays full");
    expect(last && last->duration_ms == 10000 && last->closed && last->is_last, "boundary slice is last");
    recorder_slice_t joined[2] = {*first, *last};
    expect(recorder_slices_ready_for_summary(joined, 2), "boundary stop can still be joined");
    recorder_session_destroy(session);
}

static void a_full_session_fits_and_the_next_millisecond_does_not(void)
{
    recorder_session_t *session = recorder_session_create(1, 10000);
    expect(recorder_session_add_ms(session, RECORDER_SESSION_MAX_SLICES * 10000u) == 0, "every slice slot can be filled");
    expect(recorder_session_slice_count(session) == RECORDER_SESSION_MAX_SLICES, "the session holds the full set");
    const recorder_slice_t *last = recorder_session_slice(session, RECORDER_SESSION_MAX_SLICES - 1);
    expect(last && last->closed && last->duration_ms == 10000 && !last->is_last, "the final slot is a full slice");
    expect(recorder_session_add_ms(session, 1) == -1, "audio past the last slot is refused");
    expect(recorder_session_slice_count(session) == RECORDER_SESSION_MAX_SLICES, "the refusal keeps the stored slices");
    recorder_session_destroy(session);
}

static void a_full_container_rolls_into_the_next_slice(void)
{
    recorder_session_t *session = recorder_session_create(7, 480000);
    expect(recorder_session_add_ms(session, 1000) == 0, "audio starts the slice");
    expect(recorder_session_roll(session) == 0, "a full container opens the next slice");
    const recorder_slice_t *first = recorder_session_slice(session, 0);
    const recorder_slice_t *second = recorder_session_slice(session, 1);
    expect(first && first->closed && !first->is_last && first->duration_ms == 1000, "the saved slice keeps its audio");
    expect(second && !second->closed && second->duration_ms == 0 && second->index == 1, "recording continues");
    expect(recorder_session_roll(session) != 0, "an empty slice is not rolled");
    recorder_session_destroy(session);
}

int main(void)
{
    splits_audio_past_the_slice_limit();
    stop_closes_the_open_slice_as_last();
    only_closed_slices_are_uploadable();
    summary_waits_for_every_slice_and_the_last_mark();
    stop_on_a_slice_boundary_drops_the_empty_tail();
    a_full_session_fits_and_the_next_millisecond_does_not();
    a_full_container_rolls_into_the_next_slice();
    return g_failed ? 1 : 0;
}
