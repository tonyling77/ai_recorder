#include "recorder_session.h"

#include <stdlib.h>

struct recorder_session {
    uint32_t session_id;
    uint32_t slice_limit_ms;
    recorder_slice_t slices[RECORDER_SESSION_MAX_SLICES];
    size_t count;
    size_t pending_samples;
    bool stopped;
};

static int open_slice(recorder_session_t *session)
{
    if (session->count >= RECORDER_SESSION_MAX_SLICES) {
        return -1;
    }
    recorder_slice_t *slice = &session->slices[session->count];
    slice->session_id = session->session_id;
    slice->index = (uint16_t)session->count;
    slice->duration_ms = 0;
    slice->closed = false;
    slice->is_last = false;
    session->count++;
    return 0;
}

recorder_session_t *recorder_session_create(uint32_t session_id, uint32_t slice_limit_ms)
{
    if (slice_limit_ms == 0) {
        return NULL;
    }
    recorder_session_t *session = calloc(1, sizeof(*session));
    if (!session) {
        return NULL;
    }
    session->session_id = session_id;
    session->slice_limit_ms = slice_limit_ms;
    return session;
}

void recorder_session_destroy(recorder_session_t *session)
{
    free(session);
}

int recorder_session_add_ms(recorder_session_t *session, uint32_t elapsed_ms)
{
    if (!session || session->stopped) {
        return -1;
    }
    if (session->count == 0 && open_slice(session) != 0) {
        return -1;
    }
    while (elapsed_ms > 0) {
        recorder_slice_t *current = &session->slices[session->count - 1];
        uint32_t room = session->slice_limit_ms - current->duration_ms;
        if (elapsed_ms < room) {
            current->duration_ms += elapsed_ms;
            return 0;
        }
        current->duration_ms = session->slice_limit_ms;
        current->closed = true;
        elapsed_ms -= room;
        if (open_slice(session) != 0) {
            return elapsed_ms == 0 ? 0 : -1;
        }
    }
    return 0;
}

int recorder_session_add_samples(recorder_session_t *session, uint32_t sample_hz, size_t sample_count)
{
    if (!session || sample_hz < 1000u || sample_hz % 1000u != 0) {
        return -1;
    }
    size_t samples_per_ms = sample_hz / 1000u;
    if (sample_count > SIZE_MAX - session->pending_samples) {
        return -1;
    }
    session->pending_samples += sample_count;
    uint32_t elapsed_ms = (uint32_t)(session->pending_samples / samples_per_ms);
    session->pending_samples %= samples_per_ms;
    if (elapsed_ms == 0) {
        return 0;
    }
    return recorder_session_add_ms(session, elapsed_ms);
}

int recorder_session_roll(recorder_session_t *session)
{
    if (!session || session->stopped || session->count == 0) {
        return -1;
    }
    recorder_slice_t *current = &session->slices[session->count - 1];
    if (current->closed || current->duration_ms == 0) {
        return -1;
    }
    current->closed = true;
    current->is_last = false;
    return open_slice(session);
}

int recorder_session_stop(recorder_session_t *session)
{
    if (!session || session->stopped || session->count == 0) {
        return -1;
    }
    recorder_slice_t *current = &session->slices[session->count - 1];
    if (!current->closed && current->duration_ms == 0 && session->count > 1) {
        session->count--;
        current = &session->slices[session->count - 1];
    }
    current->closed = true;
    current->is_last = true;
    session->stopped = true;
    return 0;
}

size_t recorder_session_slice_count(const recorder_session_t *session)
{
    return session ? session->count : 0;
}

const recorder_slice_t *recorder_session_slice(const recorder_session_t *session, size_t index)
{
    if (!session || index >= session->count) {
        return NULL;
    }
    return &session->slices[index];
}

bool recorder_slice_uploadable(const recorder_slice_t *slice)
{
    return slice && slice->closed;
}

bool recorder_slices_ready_for_summary(const recorder_slice_t *slices, size_t count)
{
    if (!slices || count == 0 || count > RECORDER_SESSION_MAX_SLICES) {
        return false;
    }
    bool seen[RECORDER_SESSION_MAX_SLICES] = {0};
    for (size_t i = 0; i < count; i++) {
        const recorder_slice_t *slice = &slices[i];
        if (!slice->closed || slice->index >= count || seen[slice->index]) {
            return false;
        }
        if (slice->is_last != (slice->index + 1 == count)) {
            return false;
        }
        seen[slice->index] = true;
    }
    return true;
}
