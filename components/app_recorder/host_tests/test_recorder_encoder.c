#include "recorder_encoder.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void one_frame_becomes_a_24kbit_packet(void)
{
    int16_t pcm[RECORDER_FRAME_SAMPLES];
    uint8_t packet[RECORDER_OPUS_FRAME_BYTES + 16];
    size_t packet_len = 0;
    for (size_t i = 0; i < RECORDER_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }

    recorder_encoder_t *encoder = recorder_encoder_create();
    expect(encoder != NULL, "encoder created");
    /* The frame length is the one the Xiaozhi cloud expects: it counts the
     * frames it receives as frames of 60 ms. */
    expect(RECORDER_FRAME_MS == 60, "the frame is 60 ms, the length the cloud counts");
    expect(RECORDER_FRAME_SAMPLES == 960, "60 ms at 16 kHz");
    expect(recorder_encoder_encode(encoder, pcm, RECORDER_FRAME_SAMPLES, packet, sizeof(packet), &packet_len) == 0,
           "encode one frame");
    expect(packet_len == RECORDER_OPUS_FRAME_BYTES, "24 kbit/s for 60 ms is 180 bytes");
    expect(memcmp(packet, pcm, packet_len) != 0, "packet is not the pcm bytes");
    recorder_encoder_destroy(encoder);
}

static void the_packet_is_wrapped_as_ogg_opus(void)
{
    int16_t pcm[RECORDER_FRAME_SAMPLES];
    uint8_t packet[RECORDER_OPUS_FRAME_BYTES + 16];
    size_t packet_len = 0;
    const uint8_t *ogg = NULL;
    size_t ogg_len = 0;
    for (size_t i = 0; i < RECORDER_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)((i % 32) * 200);
    }

    recorder_encoder_t *encoder = recorder_encoder_create();
    recorder_ogg_t *container = recorder_ogg_create();
    expect(recorder_encoder_encode(encoder, pcm, RECORDER_FRAME_SAMPLES, packet, sizeof(packet), &packet_len) == 0,
           "encode before wrapping");
    expect(recorder_ogg_write(container, packet, packet_len, 1) == 0, "wrap the only frame");
    expect(recorder_ogg_bytes(container, &ogg, &ogg_len) == 0, "read the container");
    expect(ogg && ogg_len > 8 && memcmp(ogg, "OggS", 4) == 0, "ogg page");

    const uint8_t *head = NULL;
    for (size_t i = 0; i + 19 < ogg_len; i++) {
        if (memcmp(ogg + i, "OpusHead", 8) == 0) {
            head = ogg + i;
            break;
        }
    }
    uint32_t rate = 0;
    if (head) {
        rate = (uint32_t)head[12] | ((uint32_t)head[13] << 8) | ((uint32_t)head[14] << 16) |
               ((uint32_t)head[15] << 24);
    }
    expect(head && head[8] == 1 && head[9] == 1 && rate == RECORDER_CAPTURE_HZ, "OpusHead is 16 kHz mono");
    expect(ogg_len < RECORDER_FRAME_SAMPLES * sizeof(int16_t), "ogg frame is smaller than pcm");
    int found_packet = 0;
    if (ogg && packet_len > 0 && ogg_len >= packet_len) {
        for (size_t i = 0; i + packet_len <= ogg_len; i++) {
            if (memcmp(ogg + i, packet, packet_len) == 0) {
                found_packet = 1;
                break;
            }
        }
    }
    expect(found_packet, "the opus packet is inside the container");
    recorder_ogg_destroy(container);
    recorder_encoder_destroy(encoder);
}

static void a_frame_longer_than_a_segment_is_still_written(void)
{
    /* The Opus frame of 60 ms can weigh more than the 255 bytes that fit in
     * one segment of an Ogg page. The format cuts such a packet over
     * several segments, and the writer has to do that. */
    uint8_t packet[300];
    const uint8_t *ogg = NULL;
    size_t ogg_len = 0;
    recorder_ogg_t *container = recorder_ogg_create();

    memset(packet, 0x5a, sizeof(packet));
    expect(recorder_ogg_write(container, packet, sizeof(packet), 1) == 0, "a long packet is written");
    expect(recorder_ogg_bytes(container, &ogg, &ogg_len) == 0 && ogg != NULL, "read the container");
    /* The page carries the packet in two segments: 255 bytes and the rest. */
    int found = 0;
    for (size_t i = 0; i + 28 <= ogg_len; i++) {
        if (memcmp(ogg + i, "OggS", 4) == 0) {
            uint8_t segments = ogg[i + 26];
            if (segments == 2 && ogg[i + 27] == 255 && ogg[i + 28] == sizeof(packet) - 255) {
                found = 1;
                break;
            }
        }
    }
    expect(found, "the long packet is cut over two segments");
    recorder_ogg_destroy(container);
}

static void eight_minutes_stays_one_closable_slice(void)
{
    uint8_t packet[RECORDER_OPUS_FRAME_BYTES];
    const size_t frames = RECORDER_SLICE_LIMIT_MS / RECORDER_FRAME_MS;
    const uint8_t *ogg = NULL;
    size_t ogg_len = 0;
    recorder_ogg_t *container = recorder_ogg_create();

    memset(packet, 0x11, sizeof(packet));
    expect(frames == 8000, "480 seconds is 8000 frames of 60 ms");
    for (size_t i = 0; i < frames; i++) {
        if (recorder_ogg_write(container, packet, sizeof(packet), 0) != 0) {
            expect(0, "an eight-minute slice fits in the container");
            recorder_ogg_destroy(container);
            return;
        }
    }
    expect(recorder_ogg_end(container) == 0, "the full slice can close");
    expect(recorder_ogg_bytes(container, &ogg, &ogg_len) == 0 && ogg_len > 1024 * 1024, "the slice holds the audio");
    recorder_ogg_destroy(container);
}

int main(void)
{
    one_frame_becomes_a_24kbit_packet();
    the_packet_is_wrapped_as_ogg_opus();
    a_frame_longer_than_a_segment_is_still_written();
    eight_minutes_stays_one_closable_slice();
    return g_failed ? 1 : 0;
}
