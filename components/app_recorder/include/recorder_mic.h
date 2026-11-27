#pragma once

#include <stdint.h>

/* Get the microphone ready, so the first press of Record does not pay for
 * it. Call once at the start of the board. */
int recorder_mic_prepare(void);
int recorder_mic_start(void);
int recorder_mic_stop(void);
uint32_t recorder_mic_elapsed_ms(void);
const char *recorder_mic_failure(void);
int recorder_mic_fault(void);
