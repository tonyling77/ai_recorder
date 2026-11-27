#pragma once

#include "recorder_session.h"

#include <stddef.h>
#include <stdint.h>

#define RECORDER_CAPTURE_HZ 16000u

int recorder_capture_push(recorder_session_t *session, const int16_t *pcm, size_t sample_count);
