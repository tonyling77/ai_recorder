#include "recorder_control.h"
#include "recorder_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void microphone_samples_are_saved_when_recording_stops(void)
{
    char root[] = "/tmp/ai-recorder-control-XXXXXX";
    int16_t pcm[RECORDER_FRAME_SAMPLES];
    uint8_t *audio = malloc(64 * 1024);
    size_t audio_len = 0;

    for (size_t i = 0; i < RECORDER_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }
    expect(mkdtemp(root) != NULL && audio != NULL, "fixtures");
    recorder_control_t *control = recorder_control_open(root);
    expect(control != NULL && !recorder_control_recording(control), "idle");
    expect(recorder_control_feed(control, pcm, RECORDER_FRAME_SAMPLES) != 0, "idle ignores the microphone");
    expect(recorder_control_start(control) == 0 && recorder_control_recording(control), "record starts");
    expect(recorder_control_start(control) != 0, "a second start does not open another take");
    expect(recorder_control_feed(control, pcm, RECORDER_FRAME_SAMPLES) == 0, "microphone samples enter the take");
    expect(recorder_control_stop(control) == 0 && !recorder_control_recording(control), "stop");
    expect(recorder_control_get(control, 1, 0, audio, 64 * 1024, &audio_len) == 0, "the take was saved");
    expect(audio_len > 4 && memcmp(audio, "OggS", 4) == 0, "the saved file is ogg opus");

    recorder_control_close(control);
    free(audio);
}

static void existing_sessions_advance_the_next_id(void)
{
    char root[] = "/tmp/ai-recorder-next-id-XXXXXX";
    char existing[512];
    int16_t pcm[RECORDER_FRAME_SAMPLES] = {0};

    expect(mkdtemp(root) != NULL, "next id fixture");
    snprintf(existing, sizeof(existing), "%s/7", root);
    expect(mkdir(existing, 0700) == 0, "existing session directory");

    recorder_control_t *control = recorder_control_open(root);
    expect(control != NULL, "control opens with existing sessions");
    expect(recorder_control_start(control) == 0, "next take starts");
    expect(recorder_control_feed(control, pcm, RECORDER_FRAME_SAMPLES) == 0, "next take receives audio");
    expect(recorder_control_stop(control) == 0, "next take stops");
    expect(recorder_control_last_session(control) == 8, "existing session id is not overwritten");

    recorder_control_close(control);
}

static int meta_duration_ms(const char *root, unsigned session_id, unsigned index, unsigned *out)
{
    char path[512];
    FILE *meta;

    snprintf(path, sizeof(path), "%s/%u/%u.meta", root, session_id, index);
    meta = fopen(path, "r");
    if (!meta || fscanf(meta, "duration_ms=%u", out) != 1) {
        if (meta) {
            fclose(meta);
        }
        return -1;
    }
    fclose(meta);
    return 0;
}

static void a_stopped_take_can_be_read_back(void)
{
    char root[] = "/tmp/ai-recorder-readback-XXXXXX";
    int16_t pcm[RECORDER_FRAME_SAMPLES];
    uint8_t *audio = malloc(64 * 1024);
    size_t audio_len = 0;
    unsigned duration_ms = 0;

    for (size_t i = 0; i < RECORDER_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }
    expect(mkdtemp(root) != NULL && audio != NULL, "readback fixtures");
    recorder_control_t *control = recorder_control_open(root);
    expect(recorder_control_start(control) == 0, "readback starts");
    expect(recorder_control_feed(control, pcm, RECORDER_FRAME_SAMPLES) == 0, "one frame is fed");
    expect(recorder_control_stop(control) == 0, "readback stops");
    expect(recorder_control_get(control, 1, 0, audio, 64 * 1024, &audio_len) == 0, "the slice can be read back");
    expect(audio_len > 4 && memcmp(audio, "OggS", 4) == 0, "the slice is ogg opus");
    char sha[65];
    expect(meta_duration_ms(root, 1, 0, &duration_ms) == 0 && duration_ms == RECORDER_FRAME_MS,
           "the saved duration matches the samples");
    expect(recorder_control_sha256(control, 1, 0, sha) == 0 && strlen(sha) == 64, "the saved slice has a checksum");
    expect(recorder_control_last_session(control) == 1, "the saved session id is 1");
    recorder_control_close(control);
    free(audio);
}

static void shown_time_follows_samples(void)
{
    char root[] = "/tmp/ai-recorder-clock-XXXXXX";
    int16_t pcm[RECORDER_FRAME_SAMPLES] = {0};
    char clock[8];
    uint32_t elapsed_ms = 0;
    int fed = 0;

    expect(mkdtemp(root) != NULL, "clock fixtures");
    recorder_control_t *control = recorder_control_open(root);
    expect(recorder_control_start(control) == 0, "clock record starts");
    /* One second of audio. It is fed in the frames the recorder uses, plus
     * the rest, because a second is not a whole number of 60 ms frames. */
    size_t wanted = RECORDER_CAPTURE_HZ;
    size_t given = 0;
    while (given < wanted) {
        size_t chunk = wanted - given;
        if (chunk > RECORDER_FRAME_SAMPLES) {
            chunk = RECORDER_FRAME_SAMPLES;
        }
        if (recorder_control_feed(control, pcm, chunk) != 0) {
            fed = -1;
        }
        given += chunk;
    }
    expect(fed == 0, "one second of samples");
    expect(recorder_control_elapsed_ms(control, &elapsed_ms) == 0 && elapsed_ms == 1000,
           "one second of samples is 1000 ms");
    expect(recorder_clock_format(elapsed_ms, clock, sizeof(clock)) == 0 && strcmp(clock, "00:01") == 0,
           "the screen shows 00:01");
    recorder_control_stop(control);
    recorder_control_close(control);
}

static void storage_unavailable_refuses_a_new_take(void)
{
    char root[] = "/tmp/ai-recorder-missing-XXXXXX";
    uint8_t audio[64];
    size_t audio_len = 0;
    struct stat st;

    expect(mkdtemp(root) != NULL, "unique path");
    expect(rmdir(root) == 0, "root is absent");
    recorder_control_t *control = recorder_control_open(root);
    expect(control != NULL, "control can name a missing root");
    expect(recorder_control_start(control) != 0, "start is refused");
    expect(!recorder_control_recording(control), "no take is open");
    expect(recorder_control_get(control, 1, 0, audio, sizeof(audio), &audio_len) != 0, "no session was saved");
    expect(stat(root, &st) != 0, "start does not create the root");
    recorder_control_close(control);
}

int main(void)
{
    microphone_samples_are_saved_when_recording_stops();
    existing_sessions_advance_the_next_id();
    storage_unavailable_refuses_a_new_take();
    a_stopped_take_can_be_read_back();
    shown_time_follows_samples();
    return g_failed ? 1 : 0;
}
