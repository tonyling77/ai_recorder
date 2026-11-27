#pragma once

#include "recorder_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct recorder_library recorder_library_t;

typedef struct {
    uint32_t session_id;
    uint32_t duration_ms;
    bool open;
    bool selected;
    char recorded_at[16];
} recorder_file_row_t;

recorder_library_t *recorder_library_create(void);
void recorder_library_destroy(recorder_library_t *library);
/* Drop every row and keep the library itself.
 *
 * The library is read by the screen while the panel is open, so replacing it
 * with a new one would pull the rows out from under the screen. Clearing it
 * keeps the pointer valid. */
void recorder_library_clear(recorder_library_t *library);
int recorder_library_add(recorder_library_t *library, uint32_t session_id, uint32_t duration_ms, bool open);
int recorder_library_set_recorded_at(recorder_library_t *library, uint32_t session_id, const char *stamp);
size_t recorder_library_count(const recorder_library_t *library);
const recorder_file_row_t *recorder_library_at(const recorder_library_t *library, size_t index);
int recorder_library_select_one(recorder_library_t *library, uint32_t session_id);
int recorder_library_toggle(recorder_library_t *library, uint32_t session_id);
int recorder_library_delete_selected(recorder_library_t *library, recorder_store_t *store, size_t *removed);
int recorder_library_finish(recorder_library_t *library, uint32_t session_id, uint32_t duration_ms);

typedef struct {
    bool multi;
    bool confirm_pending;
} recorder_files_t;

void recorder_files_init(recorder_files_t *files);
int recorder_files_set_multi(recorder_files_t *files, bool enabled);
int recorder_files_tap(recorder_files_t *files, recorder_library_t *library, uint32_t session_id);
int recorder_files_request_delete(recorder_files_t *files, const recorder_library_t *library, char *prompt,
                                  size_t prompt_len);
int recorder_files_confirm_delete(recorder_files_t *files, recorder_library_t *library, recorder_store_t *store,
                                  size_t *removed);
