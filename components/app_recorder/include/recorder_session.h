#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECORDER_SLICE_LIMIT_MS 480000u
#define RECORDER_SESSION_MAX_SLICES 128

typedef struct {
    uint32_t session_id;
    uint16_t index;
    uint32_t duration_ms;
    bool closed;
    bool is_last;
    char recorded_at[16];
} recorder_slice_t;

typedef struct recorder_session recorder_session_t;

recorder_session_t *recorder_session_create(uint32_t session_id, uint32_t slice_limit_ms);
void recorder_session_destroy(recorder_session_t *session);
int recorder_session_add_ms(recorder_session_t *session, uint32_t elapsed_ms);
int recorder_session_add_samples(recorder_session_t *session, uint32_t sample_hz, size_t sample_count);
int recorder_session_stop(recorder_session_t *session);
int recorder_session_roll(recorder_session_t *session);
size_t recorder_session_slice_count(const recorder_session_t *session);
const recorder_slice_t *recorder_session_slice(const recorder_session_t *session, size_t index);
bool recorder_slice_uploadable(const recorder_slice_t *slice);
bool recorder_slices_ready_for_summary(const recorder_slice_t *slices, size_t count);
