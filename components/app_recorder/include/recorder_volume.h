#pragma once

/* Mount the existing /nand LittleFS and create the recordings directory.
 * A failed mount leaves the volume unchanged. */
int recorder_volume_prepare(void);
const char *recorder_volume_failure(void);
