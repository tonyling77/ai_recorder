#include "provision_qr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static bool center_black(int x, int y, void *ctx)
{
    int modules = *(int *)ctx;
    (void)modules;
    return x == 0 && y == 0;
}

static int decode_qoi(const uint8_t *qoi, size_t size, uint8_t *pixels, int side)
{
    size_t i = 14;
    int count = side * side;
    int written = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    if (size < 22 || memcmp(qoi, "qoif", 4) != 0) {
        return -1;
    }
    while (written < count && i < size) {
        uint8_t op = qoi[i++];
        int run = 1;
        if (op == 0xfe) {
            if (i + 3 > size) {
                return -1;
            }
            r = qoi[i++];
            g = qoi[i++];
            b = qoi[i++];
        } else if ((op & 0xc0) == 0xc0) {
            run = (op & 0x3f) + 1;
        } else {
            return -1;
        }
        while (run-- > 0 && written < count) {
            pixels[written * 3] = r;
            pixels[written * 3 + 1] = g;
            pixels[written * 3 + 2] = b;
            written++;
        }
    }
    return written == count ? 0 : -1;
}

static void a_single_black_module_has_a_white_quiet_zone(void)
{
    int modules = 1;
    size_t need = provision_qoi_encode(1, center_black, &modules, 1, 1, NULL, 0);
    uint8_t *qoi = malloc(need);
    uint8_t pixels[9 * 3];
    expect(need > 22, "the QOI size is reported before allocation");
    expect(qoi && provision_qoi_encode(1, center_black, &modules, 1, 1, qoi, need) == need, "the image fits");
    expect(qoi && qoi[4] == 0 && qoi[5] == 0 && qoi[6] == 0 && qoi[7] == 3, "width is 3");
    expect(qoi && qoi[8] == 0 && qoi[9] == 0 && qoi[10] == 0 && qoi[11] == 3, "height is 3");
    expect(qoi && qoi[12] == 3 && qoi[13] == 0, "the image is opaque RGB");
    expect(qoi && decode_qoi(qoi, need, pixels, 3) == 0, "the QOI decodes");
    expect(pixels[0] == 255 && pixels[4 * 3] == 0 && pixels[8 * 3] == 255, "only the center module is black");
    expect(provision_qoi_encode(0, center_black, &modules, 1, 1, qoi, need) == 0, "no modules is rejected");
    expect(provision_qoi_encode(1, center_black, &modules, 1, 1, qoi, 8) == 0, "a short buffer is rejected");
    free(qoi);
}

static int first_black(const uint8_t *pixels, int side)
{
    for (int i = 0; i < side * side; i++) {
        if (pixels[i * 3] == 0) {
            return i;
        }
    }
    return -1;
}

static void a_provision_payload_encodes_a_qr(void)
{
    const char *payload = "NP:PROV_7e2d10|522655ab|b";
    size_t need = provision_qr_qoi(payload, NULL, 0);
    uint8_t *qoi = malloc(need);
    uint8_t *again;
    uint8_t *pixels;
    int side;
    int black;
    int scale;
    int modules;
    int origin;
    expect(provision_qr_qoi(NULL, NULL, 0) == 0, "missing text is rejected");
    expect(need > 22 && qoi, "the QR image has a size");
    expect(provision_qr_qoi(payload, qoi, need) == need, "the QR image is written");
    again = malloc(need);
    expect(again && provision_qr_qoi(payload, again, need) == need && memcmp(qoi, again, need) == 0,
           "the same payload encodes the same image");
    side = (qoi[4] << 24) | (qoi[5] << 16) | (qoi[6] << 8) | qoi[7];
    pixels = malloc((size_t)side * (size_t)side * 3);
    expect(side > 0 && side <= 280 &&
               ((qoi[8] << 24) | (qoi[9] << 16) | (qoi[10] << 8) | qoi[11]) == side,
           "the QR fits the 280 pixel card");
    expect(pixels && decode_qoi(qoi, need, pixels, side) == 0, "the QR QOI decodes");
    black = pixels ? first_black(pixels, side) : -1;
    scale = black >= 0 ? (black % side) / 4 : 0;
    modules = scale > 0 ? side / scale - 8 : 0;
    origin = scale > 0 ? (4 * scale) * side + 4 * scale : 0;
    expect(scale >= 2 && side % scale == 0 && modules >= 21, "the quiet zone is four modules");
    expect(pixels && pixels[origin * 3] == 0, "the finder corner is black");
    expect(pixels && pixels[((4 * scale + scale) * side + (4 * scale + scale)) * 3] == 255,
           "the finder interior is white");
    free(pixels);
    free(again);
    free(qoi);
}

int main(void)
{
    a_single_black_module_has_a_white_quiet_zone();
    a_provision_payload_encodes_a_qr();
    if (g_failed) {
        fprintf(stderr, "%d provision QR checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
