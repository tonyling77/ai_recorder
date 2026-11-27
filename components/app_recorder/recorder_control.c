#include "recorder_control.h"

#include "recorder_stamp.h"
#include "recorder_store_lock.h"
#include "recorder_take.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct recorder_control {
    recorder_store_t *store;
    recorder_library_t *library;
    recorder_take_t *take;
    uint32_t next_id;
};

/* The recordings tree is shared with the upload and the UI, so even this
 * directory scan has to run under the store lock. */
static uint32_t next_session_id(const char *root)
{
    DIR *dir;
    uint32_t maximum = 0;
    if (recorder_store_lock_acquire(RECORDER_STORE_LOCK_TIMEOUT_MS) != 0) {
        return 1;
    }
    dir = opendir(root);
    if (!dir) {
        recorder_store_lock_release();
        return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long value;
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        value = strtoul(entry->d_name, &end, 10);
        if (end && *end == '\0' && value <= UINT32_MAX && (uint32_t)value > maximum) {
            maximum = (uint32_t)value;
        }
    }
    closedir(dir);
    recorder_store_lock_release();
    return maximum < UINT32_MAX ? maximum + 1u : 0u;
}

recorder_control_t *recorder_control_open(const char *root)
{
    if (!root || root[0] == '\0') {
        return NULL;
    }
    recorder_control_t *control = calloc(1, sizeof(*control));
    if (!control) {
        return NULL;
    }
    control->store = recorder_store_open(root);
    control->library = recorder_library_create();
    control->next_id = next_session_id(root);
    if (!control->store || !control->library) {
        recorder_control_close(control);
        return NULL;
    }
    return control;
}

void recorder_control_close(recorder_control_t *control)
{
    if (!control) {
        return;
    }
    recorder_take_destroy(control->take);
    recorder_library_destroy(control->library);
    recorder_store_close(control->store);
    free(control);
}

bool recorder_control_recording(const recorder_control_t *control)
{
    return control && control->take != NULL;
}

int recorder_control_start(recorder_control_t *control)
{
    if (!control || control->take || recorder_store_ready(control->store) != 0) {
        return -1;
    }
    control->take = recorder_take_start(control->store, control->library, control->next_id, RECORDER_SLICE_LIMIT_MS);
    if (!control->take) {
        return -1;
    }
    char stamp[16];
    if (recorder_stamp_now(stamp, sizeof(stamp)) == 0) {
        (void)recorder_store_set_recorded_at(control->store, control->next_id, stamp);
    }
    control->next_id++;
    return 0;
}

int recorder_control_feed(recorder_control_t *control, const int16_t *pcm, size_t sample_count)
{
    if (!control || !control->take) {
        return -1;
    }
    return recorder_take_push(control->take, pcm, sample_count);
}

int recorder_control_elapsed_ms(const recorder_control_t *control, uint32_t *out_ms)
{
    if (!control || !out_ms) {
        return -1;
    }
    return recorder_take_elapsed_ms(control->take, out_ms);
}

int recorder_clock_format(uint32_t elapsed_ms, char *out, size_t cap)
{
    unsigned total_s;
    unsigned mins;
    unsigned secs;
    if (!out || cap < 6) {
        return -1;
    }
    total_s = elapsed_ms / 1000u;
    mins = total_s / 60u;
    secs = total_s % 60u;
    if (mins > 99) {
        return -1;
    }
    snprintf(out, cap, "%02u:%02u", mins, secs);
    return 0;
}

int recorder_control_stop(recorder_control_t *control)
{
    int stopped;
    if (!control || !control->take) {
        return -1;
    }
    stopped = recorder_take_stop(control->take);
    recorder_take_destroy(control->take);
    control->take = NULL;
    return stopped;
}

int recorder_control_get(recorder_control_t *control, uint32_t session_id, uint16_t index, uint8_t *out, size_t cap,
                         size_t *out_len)
{
    if (!control) {
        return -1;
    }
    return recorder_store_get(control->store, session_id, index, out, cap, out_len);
}

int recorder_control_sha256(recorder_control_t *control, uint32_t session_id, uint16_t index, char sha[65])
{
    if (!control) {
        return -1;
    }
    return recorder_store_sha256(control->store, session_id, index, sha);
}

uint32_t recorder_control_last_session(const recorder_control_t *control)
{
    if (!control || control->next_id <= 1) {
        return 0;
    }
    return control->next_id - 1;
}
