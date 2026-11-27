#pragma once

#include "recorder_capture.h"

#include <stddef.h>
#include <stdint.h>

/* The length of one Opus frame.
 *
 * The Xiaozhi protocol sends raw Opus frames and the server counts them as
 * frames: the upstream client sends 60 ms, and the server decodes with a
 * 960 sample buffer (60 ms at 16 kHz) and does not adapt to the
 * frame_duration in the handshake.
 *
 * A 20 ms frame made the server treat every fifteen packets as one
 * sentence: 900 ms of speech with the protocol's frame length, but only
 * 300 ms with a shorter one. The speech was cut into small pieces and the
 * transcription came back as fragments, so the agent had no fields to
 * read and the board said the minutes were unfinished.
 *
 * So the frame length is 60 ms, the same as the upstream client. */
#define RECORDER_FRAME_MS 60u
#define RECORDER_FRAME_SAMPLES (RECORDER_CAPTURE_HZ * RECORDER_FRAME_MS / 1000u)
#define RECORDER_OPUS_BITRATE 24000
#define RECORDER_OPUS_FRAME_BYTES (RECORDER_OPUS_BITRATE / 8 * RECORDER_FRAME_MS / 1000)

/* The room one Opus frame needs.
 *
 * The encoder asks for more room than the frame weighs at the configured
 * bit rate: at 60 ms it wanted 280 bytes for a frame that should weigh 180.
 * The buffer is a round number above that, so a change of the frame length
 * or of the bit rate does not quietly break the recording.
 *
 * The buffer belongs to the recorder, not to the stack of the task that
 * records: it is half a kilobyte, and the task that records has a stack of
 * its own to carry the write path. */
#define RECORDER_OPUS_FRAME_MAX 512

typedef struct recorder_encoder recorder_encoder_t;

recorder_encoder_t *recorder_encoder_create(void);
void recorder_encoder_destroy(recorder_encoder_t *encoder);
int recorder_encoder_encode(recorder_encoder_t *encoder, const int16_t *pcm, size_t sample_count,
                            uint8_t *out, size_t out_cap, size_t *out_len);

typedef struct recorder_ogg recorder_ogg_t;

recorder_ogg_t *recorder_ogg_create(void);
void recorder_ogg_destroy(recorder_ogg_t *ogg);
int recorder_ogg_write(recorder_ogg_t *ogg, const uint8_t *packet, size_t packet_len, int is_last);
int recorder_ogg_fits(const recorder_ogg_t *ogg, size_t packet_len);
void recorder_ogg_set_limit(size_t limit);
int recorder_ogg_end(recorder_ogg_t *ogg);
int recorder_ogg_bytes(const recorder_ogg_t *ogg, const uint8_t **data, size_t *len);
