#pragma once

#include "recorder_session.h"

#include <stddef.h>
#include <stdint.h>

typedef struct recorder_store recorder_store_t;

typedef struct {
    uint32_t session_id;
    uint32_t duration_ms;
    bool pending;
    char recorded_at[16];
} recorder_session_info_t;

recorder_store_t *recorder_store_open(const char *root);
void recorder_store_close(recorder_store_t *store);
int recorder_store_ready(const recorder_store_t *store);
int recorder_store_put(recorder_store_t *store, const recorder_slice_t *slice, const uint8_t *data,
                       size_t len);
int recorder_store_set_recorded_at(recorder_store_t *store, uint32_t session_id, const char *stamp);
int recorder_store_recorded_at(recorder_store_t *store, uint32_t session_id, char *out, size_t cap);
int recorder_store_get(recorder_store_t *store, uint32_t session_id, uint16_t index, uint8_t *out,
                       size_t cap, size_t *out_len);
int recorder_store_sha256(recorder_store_t *store, uint32_t session_id, uint16_t index, char sha[65]);
int recorder_store_duration_ms(recorder_store_t *store, uint32_t session_id, uint16_t index, uint32_t *out_ms);
int recorder_store_is_last(recorder_store_t *store, uint32_t session_id, uint16_t index);
int recorder_store_is_uploaded(recorder_store_t *store, uint32_t session_id, uint16_t index);
int recorder_store_mark_uploaded(recorder_store_t *store, uint32_t session_id, uint16_t index);
int recorder_store_inventory(recorder_store_t *store, uint32_t *saved_sessions, uint32_t *pending_sessions);
int recorder_store_list_sessions(recorder_store_t *store, recorder_session_info_t *out, size_t cap,
                                 size_t *out_count);
/* Read the recordings tree in one pass.
 *
 * The counts and the list come out of the same pass, because every recording
 * opens its meta file and its acknowledgement file, and opening a file on
 * this flash costs a moment. */
int recorder_store_scan(recorder_store_t *store, recorder_session_info_t *out, size_t cap, size_t *out_count,
                        uint32_t *saved_sessions, uint32_t *pending_sessions);
/* Read everything about one slice with one pass over its files.
 *
 * The separate queries each open the meta file of the slice, and opening a
 * file on this flash costs a moment. A listing that asks every question on
 * its own turns into seconds of waiting, so the slice is read in one pass. */
typedef struct {
    uint32_t duration_ms;
    bool is_last;
    bool uploaded;
    bool readable;
    char recorded_at[16];
} recorder_slice_info_t;

int recorder_store_slice_info(recorder_store_t *store, uint32_t session_id, uint16_t index,
                               recorder_slice_info_t *out);
/* Whether the slice was acknowledged as uploaded.
 *
 * The acknowledgement file holds the checksum of the recording that was
 * uploaded. The metadata of the slice holds the checksum of the recording
 * that is on the device, so the two files answer the question without
 * reading the recording itself: reading a long recording on this flash
 * takes seconds, and the caller holds the lock while it reads.
 *
 * A slice whose metadata carries no checksum is counted as uploaded when it
 * has an acknowledgement. The upload itself always compares the checksum of
 * the recording, so a recording that changed after its upload is sent again.
 *
 * Returns 1 when the slice was uploaded, 0 when it was not, -1 on error. */
int recorder_store_acknowledged(recorder_store_t *store, uint32_t session_id, uint16_t index, const char *sha);
int recorder_store_delete_session(recorder_store_t *store, uint32_t session_id);
