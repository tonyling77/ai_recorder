#include "recorder_library.h"
#include "recorder_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void single_select_replaces_the_previous_row(void)
{
    recorder_library_t *library = recorder_library_create();
    expect(recorder_library_add(library, 1, 1000, false) == 0, "first file");
    expect(recorder_library_add(library, 2, 2000, false) == 0, "second file");
    expect(recorder_library_select_one(library, 1) == 0, "select the first");
    expect(recorder_library_select_one(library, 2) == 0, "select the second");
    expect(recorder_library_at(library, 0) && !recorder_library_at(library, 0)->selected, "the first is cleared");
    expect(recorder_library_at(library, 1) && recorder_library_at(library, 1)->selected, "only the second stays selected");
    recorder_library_destroy(library);
}

static void multi_select_toggles_each_row(void)
{
    recorder_library_t *library = recorder_library_create();
    expect(recorder_library_add(library, 1, 1000, false) == 0, "row");
    expect(recorder_library_toggle(library, 1) == 0, "select");
    expect(recorder_library_at(library, 0)->selected, "selected");
    expect(recorder_library_toggle(library, 1) == 0, "clear");
    expect(!recorder_library_at(library, 0)->selected, "cleared");
    recorder_library_destroy(library);
}

static void delete_removes_selected_files_and_keeps_the_open_take(void)
{
    char root[] = "/tmp/ai-recorder-library-XXXXXX";
    const uint8_t payload[] = {'a', 'b', 'c'};
    recorder_slice_t slice = {.session_id = 11, .index = 0, .duration_ms = 20, .closed = true, .is_last = true};
    uint8_t loaded[8];
    size_t loaded_len = 0;
    size_t removed = 0;

    expect(mkdtemp(root) != NULL, "temp directory");
    recorder_store_t *store = recorder_store_open(root);
    recorder_library_t *library = recorder_library_create();
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) == 0, "saved file");
    expect(recorder_library_add(library, 11, 20, false) == 0, "pending row");
    expect(recorder_library_add(library, 12, 5, true) == 0, "open row");
    expect(recorder_library_toggle(library, 11) == 0, "select pending");
    expect(recorder_library_toggle(library, 12) == 0, "select open");
    expect(recorder_library_delete_selected(library, store, &removed) == 0, "delete");
    expect(removed == 1, "one file removed");
    expect(recorder_store_get(store, 11, 0, loaded, sizeof(loaded), &loaded_len) != 0, "pending audio is gone");
    expect(recorder_library_count(library) == 1 && recorder_library_at(library, 0)->session_id == 12,
           "the open take stays");
    recorder_library_destroy(library);
    recorder_store_close(store);
}

static void delete_waits_for_confirmation_and_skips_the_open_take(void)
{
    char root[] = "/tmp/ai-recorder-confirm-XXXXXX";
    const uint8_t payload[] = {'a', 'b', 'c'};
    recorder_slice_t first = {.session_id = 3, .index = 0, .duration_ms = 20, .closed = true, .is_last = true};
    recorder_slice_t second = {.session_id = 4, .index = 0, .duration_ms = 20, .closed = true, .is_last = true};
    uint8_t loaded[8];
    size_t loaded_len = 0;
    size_t removed = 0;
    char prompt[32];
    recorder_files_t files;

    expect(mkdtemp(root) != NULL, "temp directory");
    recorder_store_t *store = recorder_store_open(root);
    recorder_library_t *library = recorder_library_create();
    recorder_files_init(&files);
    expect(recorder_store_put(store, &first, payload, sizeof(payload)) == 0, "first file");
    expect(recorder_store_put(store, &second, payload, sizeof(payload)) == 0, "second file");
    expect(recorder_library_add(library, 3, 1000, false) == 0, "closed row");
    expect(recorder_library_add(library, 4, 2000, false) == 0, "other closed row");
    expect(recorder_library_add(library, 5, 5, true) == 0, "open row");
    expect(recorder_files_tap(&files, library, 3) == 0, "single select replaces");
    expect(recorder_files_set_multi(&files, true) == 0, "multi select is separate");
    expect(recorder_files_tap(&files, library, 4) == 0, "toggle the second");
    expect(recorder_files_tap(&files, library, 5) == 0, "toggle the open take");
    expect(recorder_files_request_delete(&files, library, prompt, sizeof(prompt)) == 0, "prompt");
    expect(strcmp(prompt, "Delete 2") == 0, "prompt counts only closed rows");
    expect(recorder_store_get(store, 3, 0, loaded, sizeof(loaded), &loaded_len) == 0, "prompt does not delete");
    expect(recorder_files_confirm_delete(&files, library, store, &removed) == 0, "confirm");
    expect(removed == 2, "both closed files are removed");
    expect(recorder_store_get(store, 3, 0, loaded, sizeof(loaded), &loaded_len) != 0, "first file is gone");
    expect(recorder_library_count(library) == 1 && recorder_library_at(library, 0)->open, "open take remains");
    recorder_library_destroy(library);
    recorder_store_close(store);
}

int main(void)
{
    single_select_replaces_the_previous_row();
    multi_select_toggles_each_row();
    delete_removes_selected_files_and_keeps_the_open_take();
    delete_waits_for_confirmation_and_skips_the_open_take();
    return g_failed ? 1 : 0;
}
