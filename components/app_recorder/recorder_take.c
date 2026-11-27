#include "recorder_take.h"

#include "recorder_encoder.h"
#include "recorder_frames.h"
#include "recorder_session.h"

#include <stdbool.h>
#include <stdlib.h>

struct recorder_take {
    recorder_store_t *store;
    recorder_library_t *library;
    recorder_session_t *session;
    recorder_encoder_t *encoder;
    recorder_ogg_t *ogg;
    recorder_frames_t *frames;
    uint32_t session_id;
    uint32_t slice_limit_ms;
    uint16_t container_index;
    bool stopped;
};

static int open_container(recorder_take_t *take)
{
    recorder_frames_destroy(take->frames);
    recorder_ogg_destroy(take->ogg);
    recorder_encoder_destroy(take->encoder);
    take->frames = NULL;
    take->ogg = NULL;
    take->encoder = recorder_encoder_create();
    take->ogg = recorder_ogg_create();
    take->frames = recorder_frames_create(take->encoder, take->ogg);
    if (!take->encoder || !take->ogg || !take->frames) {
        return -1;
    }
    return 0;
}

static int save_container(recorder_take_t *take, const recorder_slice_t *slice)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!slice || !slice->closed) {
        return -1;
    }
    if (recorder_ogg_end(take->ogg) != 0) {
        return -1;
    }
    if (recorder_ogg_bytes(take->ogg, &data, &len) != 0 || len == 0) {
        return -1;
    }
    return recorder_store_put(take->store, slice, data, len);
}

static int roll_closed_slices(recorder_take_t *take)
{
    while (1) {
        const recorder_slice_t *slice = recorder_session_slice(take->session, take->container_index);
        if (!slice || !slice->closed) {
            return 0;
        }
        if (save_container(take, slice) != 0) {
            return -1;
        }
        take->container_index++;
        if (open_container(take) != 0) {
            return -1;
        }
    }
}

recorder_take_t *recorder_take_start(recorder_store_t *store, recorder_library_t *library, uint32_t session_id,
                                     uint32_t slice_limit_ms)
{
    if (!store || !library || slice_limit_ms == 0) {
        return NULL;
    }
    recorder_take_t *take = calloc(1, sizeof(*take));
    if (!take) {
        return NULL;
    }
    take->store = store;
    take->library = library;
    take->session_id = session_id;
    take->slice_limit_ms = slice_limit_ms;
    take->session = recorder_session_create(session_id, slice_limit_ms);
    if (!take->session || open_container(take) != 0 ||
        recorder_library_add(library, session_id, 0, true) != 0) {
        recorder_take_destroy(take);
        return NULL;
    }
    return take;
}

int recorder_take_push(recorder_take_t *take, const int16_t *pcm, size_t sample_count)
{
    size_t offset = 0;
    if (!take || take->stopped || (sample_count > 0 && pcm == NULL)) {
        return -1;
    }
    while (offset < sample_count) {
        const recorder_slice_t *slice = recorder_session_slice(take->session, take->container_index);
        uint32_t duration_ms = slice ? slice->duration_ms : 0;
        size_t room_samples;
        size_t chunk;
        if (duration_ms >= take->slice_limit_ms) {
            if (roll_closed_slices(take) != 0) {
                return -1;
            }
            continue;
        }
        if (slice && slice->duration_ms > 0 && !recorder_ogg_fits(take->ogg, RECORDER_OPUS_FRAME_BYTES)) {
            if (recorder_session_roll(take->session) != 0 || roll_closed_slices(take) != 0) {
                return -1;
            }
            continue;
        }
        room_samples = (size_t)(take->slice_limit_ms - duration_ms) * (RECORDER_CAPTURE_HZ / 1000u);
        chunk = sample_count - offset;
        if (chunk > room_samples) {
            chunk = room_samples;
        }
        if (chunk > RECORDER_FRAME_SAMPLES) {
            chunk = RECORDER_FRAME_SAMPLES;
        }
        if (recorder_frames_push(take->frames, take->session, pcm + offset, chunk) != 0) {
            return -1;
        }
        offset += chunk;
        if (roll_closed_slices(take) != 0) {
            return -1;
        }
    }
    return 0;
}

int recorder_take_stop(recorder_take_t *take)
{
    uint32_t duration_ms = 0;
    const recorder_slice_t *slice;
    if (!take || take->stopped) {
        return -1;
    }
    if (recorder_frames_finish(take->frames, take->session) != 0) {
        return -1;
    }
    slice = recorder_session_slice(take->session, take->container_index);
    if (slice && slice->closed) {
        if (save_container(take, slice) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < recorder_session_slice_count(take->session); i++) {
        const recorder_slice_t *item = recorder_session_slice(take->session, i);
        duration_ms += item->duration_ms;
    }
    take->stopped = true;
    return recorder_library_finish(take->library, take->session_id, duration_ms);
}

int recorder_take_elapsed_ms(const recorder_take_t *take, uint32_t *out_ms)
{
    size_t count;
    uint32_t total = 0;
    if (!take || !out_ms || !take->session) {
        return -1;
    }
    count = recorder_session_slice_count(take->session);
    for (size_t i = 0; i < count; i++) {
        const recorder_slice_t *slice = recorder_session_slice(take->session, i);
        if (!slice) {
            return -1;
        }
        total += slice->duration_ms;
    }
    *out_ms = total;
    return 0;
}

void recorder_take_destroy(recorder_take_t *take)
{
    if (!take) {
        return;
    }
    recorder_frames_destroy(take->frames);
    recorder_ogg_destroy(take->ogg);
    recorder_encoder_destroy(take->encoder);
    recorder_session_destroy(take->session);
    free(take);
}
