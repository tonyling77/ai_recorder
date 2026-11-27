#include "recorder_encoder.h"
#include "recorder_library.h"
#include "recorder_take.h"

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

static void a_long_take_is_saved_as_closed_slices(void)
{
    char root[] = "/tmp/ai-recorder-take-XXXXXX";
    const size_t samples = 25000u * (RECORDER_CAPTURE_HZ / 1000u);
    int16_t *pcm = calloc(samples, sizeof(*pcm));
    uint8_t *audio = malloc(256 * 1024);
    size_t audio_len = 0;
    char sha[65];

    expect(mkdtemp(root) != NULL && pcm != NULL && audio != NULL, "fixtures");
    recorder_store_t *store = recorder_store_open(root);
    recorder_library_t *library = recorder_library_create();
    recorder_take_t *take = recorder_take_start(store, library, 9, 10000);
    expect(take != NULL, "start");
    expect(recorder_library_count(library) == 1 && recorder_library_at(library, 0)->open, "the take is open");
    expect(recorder_take_push(take, pcm, samples) == 0, "record 25 seconds");
    expect(recorder_take_stop(take) == 0, "stop");
    expect(recorder_library_count(library) == 1 && !recorder_library_at(library, 0)->open, "the take is pending");
    expect(recorder_library_at(library, 0)->duration_ms == 25000, "duration is the spoken time");

    for (uint16_t index = 0; index < 3; index++) {
        expect(recorder_store_get(store, 9, index, audio, 256 * 1024, &audio_len) == 0, "slice file exists");
        expect(audio_len > 4 && memcmp(audio, "OggS", 4) == 0, "slice is ogg");
        expect(recorder_store_sha256(store, 9, index, sha) == 0 && strlen(sha) == 64, "slice has a checksum");
    }
    expect(recorder_store_is_last(store, 9, 0) == 0, "the first slice continues");
    expect(recorder_store_is_last(store, 9, 2) == 1, "the tail is the last slice");

    recorder_take_destroy(take);
    recorder_library_destroy(library);
    recorder_store_close(store);
    free(audio);
    free(pcm);
}

static void a_full_container_continues_in_the_next_slice(void)
{
    char root[] = "/tmp/ai-recorder-roll-XXXXXX";
    const size_t samples = 3000u * (RECORDER_CAPTURE_HZ / 1000u);
    int16_t *pcm = calloc(samples, sizeof(*pcm));
    uint8_t *audio = malloc(64 * 1024);
    size_t audio_len = 0;

    expect(mkdtemp(root) != NULL && pcm != NULL && audio != NULL, "roll fixtures");
    recorder_ogg_set_limit(4096);
    recorder_store_t *store = recorder_store_open(root);
    recorder_library_t *library = recorder_library_create();
    recorder_take_t *take = recorder_take_start(store, library, 3, RECORDER_SLICE_LIMIT_MS);
    expect(take != NULL, "start under a tiny container");
    expect(recorder_take_push(take, pcm, samples) == 0, "recording continues after the container fills");
    expect(recorder_take_stop(take) == 0, "the continued take can stop");
    expect(recorder_store_get(store, 3, 0, audio, 64 * 1024, &audio_len) == 0 && audio_len > 4, "the first slice is saved");
    audio_len = 0;
    expect(recorder_store_get(store, 3, 1, audio, 64 * 1024, &audio_len) == 0 && audio_len > 4, "the next slice is saved");
    recorder_ogg_set_limit(0);
    recorder_take_destroy(take);
    recorder_library_destroy(library);
    recorder_store_close(store);
    free(audio);
    free(pcm);
}

int main(void)
{
    a_long_take_is_saved_as_closed_slices();
    a_full_container_continues_in_the_next_slice();
    return g_failed ? 1 : 0;
}
