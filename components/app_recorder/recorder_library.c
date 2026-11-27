#include "recorder_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORDER_LIBRARY_MAX 128

struct recorder_library {
    recorder_file_row_t rows[RECORDER_LIBRARY_MAX];
    size_t count;
};

static recorder_file_row_t *find_row(recorder_library_t *library, uint32_t session_id);

recorder_library_t *recorder_library_create(void)
{
    return calloc(1, sizeof(recorder_library_t));
}

void recorder_library_destroy(recorder_library_t *library)
{
    free(library);
}

void recorder_library_clear(recorder_library_t *library)
{
    if (!library) {
        return;
    }
    library->count = 0;
}

int recorder_library_add(recorder_library_t *library, uint32_t session_id, uint32_t duration_ms, bool open)
{
    if (!library || library->count >= RECORDER_LIBRARY_MAX) {
        return -1;
    }
    recorder_file_row_t *row = &library->rows[library->count++];
    row->session_id = session_id;
    row->duration_ms = duration_ms;
    row->open = open;
    row->selected = false;
    row->recorded_at[0] = '\0';
    return 0;
}

int recorder_library_set_recorded_at(recorder_library_t *library, uint32_t session_id, const char *stamp)
{
    recorder_file_row_t *row = find_row(library, session_id);
    if (!row || !stamp) {
        return -1;
    }
    snprintf(row->recorded_at, sizeof(row->recorded_at), "%s", stamp);
    return 0;
}

size_t recorder_library_count(const recorder_library_t *library)
{
    return library ? library->count : 0;
}

const recorder_file_row_t *recorder_library_at(const recorder_library_t *library, size_t index)
{
    if (!library || index >= library->count) {
        return NULL;
    }
    return &library->rows[index];
}

static recorder_file_row_t *find_row(recorder_library_t *library, uint32_t session_id)
{
    for (size_t i = 0; i < library->count; i++) {
        if (library->rows[i].session_id == session_id) {
            return &library->rows[i];
        }
    }
    return NULL;
}

int recorder_library_select_one(recorder_library_t *library, uint32_t session_id)
{
    recorder_file_row_t *row;
    if (!library) {
        return -1;
    }
    row = find_row(library, session_id);
    if (!row) {
        return -1;
    }
    for (size_t i = 0; i < library->count; i++) {
        library->rows[i].selected = false;
    }
    row->selected = true;
    return 0;
}

int recorder_library_toggle(recorder_library_t *library, uint32_t session_id)
{
    recorder_file_row_t *row;
    if (!library) {
        return -1;
    }
    row = find_row(library, session_id);
    if (!row) {
        return -1;
    }
    row->selected = !row->selected;
    return 0;
}

int recorder_library_finish(recorder_library_t *library, uint32_t session_id, uint32_t duration_ms)
{
    recorder_file_row_t *row;
    if (!library) {
        return -1;
    }
    row = find_row(library, session_id);
    if (!row) {
        return -1;
    }
    row->open = false;
    row->duration_ms = duration_ms;
    return 0;
}

int recorder_library_delete_selected(recorder_library_t *library, recorder_store_t *store, size_t *removed)
{
    size_t count = 0;
    if (!library || !store || !removed) {
        return -1;
    }
    for (size_t i = 0; i < library->count;) {
        recorder_file_row_t *row = &library->rows[i];
        if (!row->selected || row->open) {
            i++;
            continue;
        }
        if (recorder_store_delete_session(store, row->session_id) != 0) {
            return -1;
        }
        memmove(row, row + 1, (library->count - i - 1) * sizeof(*row));
        library->count--;
        count++;
    }
    *removed = count;
    return 0;
}

static size_t deletable_count(const recorder_library_t *library)
{
    size_t count = 0;
    if (!library) {
        return 0;
    }
    for (size_t i = 0; i < library->count; i++) {
        if (library->rows[i].selected && !library->rows[i].open) {
            count++;
        }
    }
    return count;
}

void recorder_files_init(recorder_files_t *files)
{
    if (!files) {
        return;
    }
    files->multi = false;
    files->confirm_pending = false;
}

int recorder_files_set_multi(recorder_files_t *files, bool enabled)
{
    if (!files) {
        return -1;
    }
    files->multi = enabled;
    files->confirm_pending = false;
    return 0;
}

int recorder_files_tap(recorder_files_t *files, recorder_library_t *library, uint32_t session_id)
{
    int result;
    if (!files) {
        return -1;
    }
    result = files->multi ? recorder_library_toggle(library, session_id)
                          : recorder_library_select_one(library, session_id);
    if (result == 0) {
        files->confirm_pending = false;
    }
    return result;
}

int recorder_files_request_delete(recorder_files_t *files, const recorder_library_t *library, char *prompt,
                                  size_t prompt_len)
{
    size_t count;
    if (!files || !prompt || prompt_len < 16) {
        return -1;
    }
    count = deletable_count(library);
    if (count == 0) {
        files->confirm_pending = false;
        prompt[0] = '\0';
        return -1;
    }
    snprintf(prompt, prompt_len, "Delete %u", (unsigned)count);
    files->confirm_pending = true;
    return 0;
}

int recorder_files_confirm_delete(recorder_files_t *files, recorder_library_t *library, recorder_store_t *store,
                                  size_t *removed)
{
    if (!files || !files->confirm_pending) {
        return -1;
    }
    files->confirm_pending = false;
    return recorder_library_delete_selected(library, store, removed);
}
