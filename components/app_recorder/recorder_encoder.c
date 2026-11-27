#include "recorder_encoder.h"

#include <opus.h>
#include <stdlib.h>

struct recorder_encoder {
    OpusEncoder *opus;
};

recorder_encoder_t *recorder_encoder_create(void)
{
    int error = OPUS_OK;
    recorder_encoder_t *encoder = calloc(1, sizeof(*encoder));
    if (!encoder) {
        return NULL;
    }
    encoder->opus = opus_encoder_create(RECORDER_CAPTURE_HZ, 1, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder->opus || error != OPUS_OK) {
        free(encoder);
        return NULL;
    }
    opus_encoder_ctl(encoder->opus, OPUS_SET_BITRATE(RECORDER_OPUS_BITRATE));
    opus_encoder_ctl(encoder->opus, OPUS_SET_VBR(0));
    return encoder;
}

void recorder_encoder_destroy(recorder_encoder_t *encoder)
{
    if (!encoder) {
        return;
    }
    opus_encoder_destroy(encoder->opus);
    free(encoder);
}

int recorder_encoder_encode(recorder_encoder_t *encoder, const int16_t *pcm, size_t sample_count,
                            uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!encoder || !encoder->opus || !pcm || !out || !out_len || sample_count != RECORDER_FRAME_SAMPLES) {
        return -1;
    }
    int written = opus_encode(encoder->opus, pcm, (int)sample_count, out, (opus_int32)out_cap);
    if (written < 0) {
        return -1;
    }
    *out_len = (size_t)written;
    return 0;
}
