#include "recorder_encoder.h"

#include <stdlib.h>

#include "encoder/impl/esp_opus_enc.h"

struct recorder_encoder {
    void *opus;
};

recorder_encoder_t *recorder_encoder_create(void)
{
    esp_opus_enc_config_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = RECORDER_OPUS_BITRATE,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = false,
    };
    recorder_encoder_t *encoder = calloc(1, sizeof(*encoder));
    if (!encoder) {
        return NULL;
    }
    if (esp_opus_enc_open(&config, sizeof(config), &encoder->opus) != ESP_AUDIO_ERR_OK || !encoder->opus) {
        free(encoder);
        return NULL;
    }
    return encoder;
}

void recorder_encoder_destroy(recorder_encoder_t *encoder)
{
    if (!encoder) {
        return;
    }
    if (encoder->opus) {
        esp_opus_enc_close(encoder->opus);
    }
    free(encoder);
}

int recorder_encoder_encode(recorder_encoder_t *encoder, const int16_t *pcm, size_t sample_count, uint8_t *out,
                            size_t out_cap, size_t *out_len)
{
    esp_audio_enc_in_frame_t in_frame = {0};
    esp_audio_enc_out_frame_t out_frame = {0};

    if (!encoder || !encoder->opus || !pcm || !out || !out_len || sample_count != RECORDER_FRAME_SAMPLES) {
        return -1;
    }
    in_frame.buffer = (uint8_t *)pcm;
    in_frame.len = (uint32_t)(sample_count * sizeof(int16_t));
    out_frame.buffer = out;
    out_frame.len = (uint32_t)out_cap;
    if (esp_opus_enc_process(encoder->opus, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK ||
        out_frame.encoded_bytes == 0 || out_frame.encoded_bytes > out_cap) {
        return -1;
    }
    *out_len = out_frame.encoded_bytes;
    return 0;
}
