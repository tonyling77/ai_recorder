#pragma once

#include "recorder_capture.h"
#include "recorder_library.h"

#include <stddef.h>
#include <stdint.h>

typedef struct recorder_take recorder_take_t;

recorder_take_t *recorder_take_start(recorder_store_t *store, recorder_library_t *library, uint32_t session_id,
                                     uint32_t slice_limit_ms);
int recorder_take_push(recorder_take_t *take, const int16_t *pcm, size_t sample_count);
int recorder_take_stop(recorder_take_t *take);
int recorder_take_elapsed_ms(const recorder_take_t *take, uint32_t *out_ms);
void recorder_take_destroy(recorder_take_t *take);
