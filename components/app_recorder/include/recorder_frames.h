#pragma once

#include "recorder_encoder.h"
#include "recorder_session.h"

#include <stddef.h>
#include <stdint.h>

typedef struct recorder_frames recorder_frames_t;

recorder_frames_t *recorder_frames_create(recorder_encoder_t *encoder, recorder_ogg_t *ogg);
void recorder_frames_destroy(recorder_frames_t *frames);
int recorder_frames_push(recorder_frames_t *frames, recorder_session_t *session, const int16_t *pcm,
                         size_t sample_count);
int recorder_frames_finish(recorder_frames_t *frames, recorder_session_t *session);
