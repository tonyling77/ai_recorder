#include "recorder_frames.h"

#include <stdlib.h>
#include <string.h>

struct recorder_frames {
    recorder_encoder_t *encoder;
    recorder_ogg_t *ogg;
    int16_t pending[RECORDER_FRAME_SAMPLES];
    size_t pending_count;
    /* The room one Opus frame needs. It belongs here, because the task that
     * records has a stack of its own to carry the write path. */
    uint8_t packet[RECORDER_OPUS_FRAME_MAX];
};

recorder_frames_t *recorder_frames_create(recorder_encoder_t *encoder, recorder_ogg_t *ogg)
{
    recorder_frames_t *frames = calloc(1, sizeof(*frames));
    if (!frames) {
        return NULL;
    }
    frames->encoder = encoder;
    frames->ogg = ogg;
    return frames;
}

void recorder_frames_destroy(recorder_frames_t *frames)
{
    free(frames);
}

int recorder_frames_push(recorder_frames_t *frames, recorder_session_t *session, const int16_t *pcm,
                         size_t sample_count)
{
    size_t offset = 0;

    if (!frames || !session || (sample_count > 0 && pcm == NULL)) {
        return -1;
    }
    if (recorder_capture_push(session, pcm, sample_count) != 0) {
        return -1;
    }
    while (offset < sample_count) {
        size_t room = RECORDER_FRAME_SAMPLES - frames->pending_count;
        size_t take = sample_count - offset;
        size_t packet_len = 0;

        if (take > room) {
            take = room;
        }
        memcpy(frames->pending + frames->pending_count, pcm + offset, take * sizeof(int16_t));
        frames->pending_count += take;
        offset += take;
        if (frames->pending_count < RECORDER_FRAME_SAMPLES) {
            continue;
        }
        if (recorder_encoder_encode(frames->encoder, frames->pending, RECORDER_FRAME_SAMPLES, frames->packet,
                                    sizeof(frames->packet), &packet_len) != 0) {
            return -1;
        }
        if (recorder_ogg_write(frames->ogg, frames->packet, packet_len, 0) != 0) {
            return -1;
        }
        frames->pending_count = 0;
    }
    return 0;
}

int recorder_frames_finish(recorder_frames_t *frames, recorder_session_t *session)
{
    size_t packet_len = 0;

    if (!frames || !session) {
        return -1;
    }
    if (frames->pending_count > 0) {
        memset(frames->pending + frames->pending_count, 0,
               (RECORDER_FRAME_SAMPLES - frames->pending_count) * sizeof(int16_t));
        if (recorder_encoder_encode(frames->encoder, frames->pending, RECORDER_FRAME_SAMPLES, frames->packet,
                                    sizeof(frames->packet), &packet_len) != 0) {
            return -1;
        }
        if (recorder_ogg_write(frames->ogg, frames->packet, packet_len, 1) != 0) {
            return -1;
        }
        frames->pending_count = 0;
    } else if (recorder_ogg_end(frames->ogg) != 0) {
        return -1;
    }
    return recorder_session_stop(session);
}
