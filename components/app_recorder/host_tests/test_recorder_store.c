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

static void a_closed_slice_round_trips(void)
{
    char root[] = "/tmp/ai-recorder-store-XXXXXX";
    recorder_slice_t slice = {
        .session_id = 7,
        .index = 0,
        .duration_ms = 20,
        .closed = true,
        .is_last = true,
    };
    const uint8_t payload[] = {'a', 'b', 'c'};
    uint8_t loaded[8] = {0};
    size_t loaded_len = 0;
    char sha[65] = {0};

    expect(mkdtemp(root) != NULL, "temp directory");
    recorder_store_t *store = recorder_store_open(root);
    slice.closed = false;
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) != 0, "an open slice is not stored");
    slice.closed = true;
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) == 0, "store the closed slice");
    expect(recorder_store_get(store, 7, 0, loaded, sizeof(loaded), &loaded_len) == 0, "load the slice");
    expect(loaded_len == sizeof(payload) && memcmp(loaded, payload, sizeof(payload)) == 0, "bytes match");
    expect(recorder_store_sha256(store, 7, 0, sha) == 0, "read the checksum");
    expect(strcmp(sha, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
           "sha256 of abc");
    uint32_t duration_ms = 0;
    expect(recorder_store_duration_ms(store, 7, 0, &duration_ms) == 0 && duration_ms == 20,
           "the saved duration can be read back");
    expect(recorder_store_is_uploaded(store, 7, 0) == 0, "new slice is pending upload");
    expect(recorder_store_mark_uploaded(store, 7, 0) == 0, "acknowledgement is persisted");
    expect(recorder_store_is_uploaded(store, 7, 0) == 1, "acknowledged slice is not pending");
    const uint8_t replacement[] = {'x', 'y', 'z'};
    expect(recorder_store_put(store, &slice, replacement, sizeof(replacement)) == 0, "replace the slice");
    expect(recorder_store_is_uploaded(store, 7, 0) == 0, "changed bytes invalidate the acknowledgement");
    uint32_t saved = 0;
    uint32_t pending = 0;
    expect(recorder_store_inventory(store, &saved, &pending) == 0, "inventory can be scanned");
    expect(saved == 1 && pending == 1, "inventory restores saved and pending sessions");
    expect(recorder_store_delete_session(store, 7) == 0, "delete the session");
    expect(recorder_store_get(store, 7, 0, loaded, sizeof(loaded), &loaded_len) != 0, "deleted bytes are gone");

    recorder_store_close(store);
}

static void sessions_are_listed_newest_first(void)
{
    char root[] = "/tmp/ai-recorder-list-XXXXXX";
    const uint8_t payload[] = {'a'};
    recorder_slice_t slice = {.closed = true};
    expect(mkdtemp(root) != NULL, "list temp directory");
    recorder_store_t *store = recorder_store_open(root);

    slice.session_id = 4;
    slice.index = 0;
    slice.duration_ms = 1200;
    slice.is_last = false;
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) == 0, "store first slice");
    slice.index = 1;
    slice.duration_ms = 800;
    slice.is_last = true;
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) == 0, "store final slice");

    slice.session_id = 9;
    slice.index = 0;
    slice.duration_ms = 3000;
    slice.is_last = true;
    expect(recorder_store_put(store, &slice, payload, sizeof(payload)) == 0, "store newest session");
    expect(recorder_store_mark_uploaded(store, 9, 0) == 0, "mark newest uploaded");

    recorder_session_info_t sessions[2] = {0};
    size_t count = 0;
    expect(recorder_store_list_sessions(store, sessions, 2, &count) == 0, "list sessions");
    expect(count == 2, "two sessions listed");
    expect(sessions[0].session_id == 9 && sessions[0].duration_ms == 3000 && !sessions[0].pending,
           "newest uploaded session metadata");
    expect(sessions[1].session_id == 4 && sessions[1].duration_ms == 2000 && sessions[1].pending,
           "older pending session metadata");
    recorder_store_close(store);
}

int main(void)
{
    a_closed_slice_round_trips();
    sessions_are_listed_newest_first();
    return g_failed ? 1 : 0;
}
