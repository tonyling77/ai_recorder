#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Send the closed slices of the recordings to the inbox.
 *
 * only holds the recordings the user picked. A NULL only means every
 * recording that is not uploaded yet, which is what the main screen does.
 * A list means only those recordings, which is what the recordings panel
 * does when the user picks rows and presses Upload. */
int recorder_uplink_send_pending(const char *root, const char *device_id, const uint32_t *only,
                                  size_t only_count);
const char *recorder_uplink_failure(void);
/* Why the upload sent nothing, when it sent nothing. NULL when it sent
 * something. */
const char *recorder_uplink_note(void);
void recorder_uplink_device_id(char *out, size_t cap);
