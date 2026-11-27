#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct recorder_control recorder_control_t;

recorder_control_t *recorder_control_open(const char *root);
void recorder_control_close(recorder_control_t *control);
bool recorder_control_recording(const recorder_control_t *control);
int recorder_control_start(recorder_control_t *control);
int recorder_control_feed(recorder_control_t *control, const int16_t *pcm, size_t sample_count);
int recorder_control_stop(recorder_control_t *control);
int recorder_control_elapsed_ms(const recorder_control_t *control, uint32_t *out_ms);
int recorder_clock_format(uint32_t elapsed_ms, char *out, size_t cap);
int recorder_control_get(recorder_control_t *control, uint32_t session_id, uint16_t index, uint8_t *out,
                         size_t cap, size_t *out_len);
int recorder_control_sha256(recorder_control_t *control, uint32_t session_id, uint16_t index, char sha[65]);
uint32_t recorder_control_last_session(const recorder_control_t *control);
