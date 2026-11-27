#include "recorder_encoder.h"

#include <stdlib.h>
#include <string.h>

/* One page per frame for 480 seconds is about 2.5 MB. 4 MB leaves room
 * for the header pages, the end page, and a packet larger than the CBR
 * target. */
#define OGG_MAX (4u * 1024u * 1024u)
#define OGG_BOS 0x02u
#define OGG_EOS 0x04u
#define OGG_SERIAL 0xA1C0DEC0u
/* The largest packet one page carries. An Opus frame is far smaller, but
 * the page has to know the bound before it copies anything. */
#define OGG_MAX_PACKET 4096u
/* The samples one frame holds. The granule of a page says where the audio
 * ends, so it has to count the samples of the frame, not a number of its
 * own. */
#define OGG_GRANULE_PER_FRAME RECORDER_FRAME_SAMPLES

struct recorder_ogg {
    uint8_t *bytes;
    size_t len;
    size_t cap;
    uint32_t sequence;
    int started;
    int finished;
    uint64_t granule;
};

static uint32_t crc_table[256];
static int crc_ready;
static size_t ogg_limit = OGG_MAX;

static void crc_init(void)
{
    if (crc_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t reg = i << 24;
        for (int bit = 0; bit < 8; bit++) {
            if (reg & 0x80000000u) {
                reg = (reg << 1) ^ 0x04c11db7u;
            } else {
                reg <<= 1;
            }
        }
        crc_table[i] = reg;
    }
    crc_ready = 1;
}

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    crc_init();
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc_table[((crc >> 24) & 0xffu) ^ data[i]];
    }
    return crc;
}

static void write_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static int ensure_cap(recorder_ogg_t *ogg, size_t extra)
{
    size_t need = ogg->len + extra;
    size_t cap = ogg->cap;
    uint8_t *grown;

    if (need <= cap) {
        return 0;
    }
    while (need > cap) {
        if (cap >= ogg_limit) {
            return -1;
        }
        cap = cap > ogg_limit / 2u ? ogg_limit : cap * 2u;
    }
    grown = realloc(ogg->bytes, cap);
    if (!grown) {
        return -1;
    }
    ogg->bytes = grown;
    ogg->cap = cap;
    return 0;
}

static int write_page(recorder_ogg_t *ogg, uint8_t header_type, uint64_t granule,
                      const uint8_t *payload, size_t payload_len)
{
    /* A page carries the packet in segments of at most 255 bytes. A packet
     * that is longer than that is cut over several segments, which is what
     * the format is for. The Opus frame of 60 ms can be longer than 255
     * bytes, so the single segment of before was not enough. */
    size_t segments;
    size_t header_len;
    size_t page_len;
    size_t remaining;
    size_t offset;

    if (payload_len == 0 || payload_len > OGG_MAX_PACKET) {
        return -1;
    }
    segments = (payload_len + 254u) / 255u;
    if (segments > 255u) {
        return -1;
    }
    header_len = 27u + segments;
    page_len = header_len + payload_len;
    if (ensure_cap(ogg, page_len) != 0) {
        return -1;
    }
    uint8_t *page = ogg->bytes + ogg->len;
    memset(page, 0, header_len);
    memcpy(page, "OggS", 4);
    page[5] = header_type;
    for (int i = 0; i < 8; i++) {
        page[6 + i] = (uint8_t)((granule >> (8 * i)) & 0xffu);
    }
    write_le32(page + 14, OGG_SERIAL);
    write_le32(page + 18, ogg->sequence++);
    page[26] = (uint8_t)segments;
    /* The table of the segments says where the packet ends: every segment
     * that is full continues the packet, and the last one ends it. */
    remaining = payload_len;
    offset = 27;
    for (size_t i = 0; i < segments; i++) {
        size_t take = remaining > 255u ? 255u : remaining;
        page[offset++] = (uint8_t)take;
        remaining -= take;
    }
    memcpy(page + header_len, payload, payload_len);
    write_le32(page + 22, ogg_crc(page, page_len));
    ogg->len += page_len;
    return 0;
}

static int write_headers(recorder_ogg_t *ogg)
{
    uint8_t head[19];
    memset(head, 0, sizeof(head));
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 1;
    write_le32(head + 12, RECORDER_CAPTURE_HZ);
    if (write_page(ogg, OGG_BOS, 0, head, sizeof(head)) != 0) {
        return -1;
    }

    const char vendor[] = "ai_recorder";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memset(tags, 0, sizeof(tags));
    memcpy(tags, "OpusTags", 8);
    write_le32(tags + 8, (uint32_t)(sizeof(vendor) - 1));
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    return write_page(ogg, 0, 0, tags, sizeof(tags));
}

void recorder_ogg_set_limit(size_t limit)
{
    ogg_limit = limit == 0 ? OGG_MAX : limit;
}

int recorder_ogg_fits(const recorder_ogg_t *ogg, size_t packet_len)
{
    size_t extra;
    size_t segments;

    if (!ogg || ogg->finished || packet_len == 0 || ogg->len > ogg_limit) {
        return 0;
    }
    segments = (packet_len + 254u) / 255u;
    extra = 27u + segments + packet_len + 27u;
    if (!ogg->started) {
        extra += 47u + 55u;
    }
    return extra <= ogg_limit - ogg->len;
}

recorder_ogg_t *recorder_ogg_create(void)
{
    recorder_ogg_t *ogg = calloc(1, sizeof(*ogg));
    if (!ogg) {
        return NULL;
    }
    ogg->cap = 1024;
    ogg->bytes = malloc(ogg->cap);
    if (!ogg->bytes) {
        free(ogg);
        return NULL;
    }
    return ogg;
}

void recorder_ogg_destroy(recorder_ogg_t *ogg)
{
    if (!ogg) {
        return;
    }
    free(ogg->bytes);
    free(ogg);
}

int recorder_ogg_write(recorder_ogg_t *ogg, const uint8_t *packet, size_t packet_len, int is_last)
{
    if (!ogg || ogg->finished || !packet || packet_len == 0) {
        return -1;
    }
    if (!ogg->started) {
        if (write_headers(ogg) != 0) {
            return -1;
        }
        ogg->started = 1;
    }
    uint8_t flags = is_last ? OGG_EOS : 0;
    ogg->granule += OGG_GRANULE_PER_FRAME;
    if (write_page(ogg, flags, ogg->granule, packet, packet_len) != 0) {
        return -1;
    }
    if (is_last) {
        ogg->finished = 1;
    }
    return 0;
}

int recorder_ogg_end(recorder_ogg_t *ogg)
{
    size_t page_len = 27;
    uint8_t *page;
    if (!ogg) {
        return -1;
    }
    if (!ogg->started || ogg->finished) {
        return 0;
    }
    if (ensure_cap(ogg, page_len) != 0) {
        return -1;
    }
    page = ogg->bytes + ogg->len;
    memset(page, 0, page_len);
    memcpy(page, "OggS", 4);
    page[5] = OGG_EOS;
    for (int i = 0; i < 8; i++) {
        page[6 + i] = (uint8_t)((ogg->granule >> (8 * i)) & 0xffu);
    }
    write_le32(page + 14, OGG_SERIAL);
    write_le32(page + 18, ogg->sequence++);
    write_le32(page + 22, ogg_crc(page, page_len));
    ogg->len += page_len;
    ogg->finished = 1;
    return 0;
}

int recorder_ogg_bytes(const recorder_ogg_t *ogg, const uint8_t **data, size_t *len)
{
    if (!ogg || !data || !len) {
        return -1;
    }
    *data = ogg->bytes;
    *len = ogg->len;
    return 0;
}
