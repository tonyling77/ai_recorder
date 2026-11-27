// SPDX-License-Identifier: Apache-2.0

#include "provision_qr.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "qrcode.h"
#else
#include "qrcodegen.h"
#endif

#define PROVISION_QR_MAX_SIDE 280
#define PROVISION_QR_QUIET 4
#define PROVISION_QR_MAX_VERSION 6

static void emit(uint8_t *out, size_t *used, size_t cap, int *overflow, uint8_t value)
{
    if (out) {
        if (*used >= cap) {
            *overflow = 1;
            return;
        }
        out[*used] = value;
    }
    (*used)++;
}

static size_t encode_qoi(int modules, provision_module_fn module, void *ctx, int quiet, int scale,
                          uint8_t *out, size_t cap)
{
    int span = modules + quiet * 2;
    size_t side = (size_t)span * (size_t)scale;
    size_t used = 0;
    int overflow = 0;
    uint8_t previous_r = 0;
    uint8_t previous_g = 0;
    uint8_t previous_b = 0;
    int run = 0;
    size_t pixel_count = side * side;
    size_t pixel_index = 0;

    emit(out, &used, cap, &overflow, 'q');
    emit(out, &used, cap, &overflow, 'o');
    emit(out, &used, cap, &overflow, 'i');
    emit(out, &used, cap, &overflow, 'f');
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 24) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 16) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 8) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)(side & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 24) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 16) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)((side >> 8) & 0xff));
    emit(out, &used, cap, &overflow, (uint8_t)(side & 0xff));
    emit(out, &used, cap, &overflow, 3);
    emit(out, &used, cap, &overflow, 0);

    for (size_t y = 0; y < side; y++) {
        for (size_t x = 0; x < side; x++) {
            int module_x = (int)(x / (size_t)scale) - quiet;
            int module_y = (int)(y / (size_t)scale) - quiet;
            int black = module_x >= 0 && module_y >= 0 && module_x < modules && module_y < modules &&
                        module(module_x, module_y, ctx);
            uint8_t r = black ? 0 : 255;
            uint8_t g = r;
            uint8_t b = r;
            pixel_index++;
            if (r == previous_r && g == previous_g && b == previous_b) {
                run++;
                if (run == 62 || pixel_index == pixel_count) {
                    emit(out, &used, cap, &overflow, (uint8_t)(0xc0 | (run - 1)));
                    run = 0;
                }
            } else {
                if (run > 0) {
                    emit(out, &used, cap, &overflow, (uint8_t)(0xc0 | (run - 1)));
                    run = 0;
                }
                emit(out, &used, cap, &overflow, 0xfe);
                emit(out, &used, cap, &overflow, r);
                emit(out, &used, cap, &overflow, g);
                emit(out, &used, cap, &overflow, b);
                previous_r = r;
                previous_g = g;
                previous_b = b;
            }
        }
    }
    for (int i = 0; i < 7; i++) {
        emit(out, &used, cap, &overflow, 0);
    }
    emit(out, &used, cap, &overflow, 1);
    if (overflow) {
        return 0;
    }
    return used;
}

size_t provision_qoi_encode(int modules, provision_module_fn module, void *ctx, int quiet, int scale,
                            uint8_t *out, size_t cap)
{
    size_t need;
    if (modules < 1 || modules > 177 || quiet < 0 || scale < 1 || scale > 32 || !module) {
        return 0;
    }
    if (modules + quiet * 2 <= 0) {
        return 0;
    }
    need = encode_qoi(modules, module, ctx, quiet, scale, NULL, 0);
    if (!out) {
        return need;
    }
    if (need == 0 || cap < need) {
        return 0;
    }
    return encode_qoi(modules, module, ctx, quiet, scale, out, cap);
}

#if defined(ESP_PLATFORM)
typedef struct {
    uint8_t *out;
    size_t cap;
    size_t written;
} provision_qr_job_t;

static bool module_from_esp(int x, int y, void *ctx)
{
    return esp_qrcode_get_module((esp_qrcode_handle_t)ctx, x, y);
}

static void encode_esp_qr(esp_qrcode_handle_t qrcode, void *user_data)
{
    provision_qr_job_t *job = user_data;
    int modules = esp_qrcode_get_size(qrcode);
    int scale = PROVISION_QR_MAX_SIDE / (modules + PROVISION_QR_QUIET * 2);
    if (scale < 2) {
        job->written = 0;
        return;
    }
    job->written = provision_qoi_encode(modules, module_from_esp, (void *)qrcode, PROVISION_QR_QUIET, scale,
                                        job->out, job->cap);
}
#else
static bool module_from_qrcodegen(int x, int y, void *ctx)
{
    return qrcodegen_getModule((const uint8_t *)ctx, x, y);
}
#endif

size_t provision_qr_qoi(const char *text, uint8_t *out, size_t cap)
{
    if (!text || text[0] == '\0') {
        return 0;
    }
#if defined(ESP_PLATFORM)
    provision_qr_job_t job = {
        .out = out,
        .cap = cap,
        .written = 0,
    };
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func_with_cb = encode_esp_qr;
    config.user_data = &job;
    config.max_qrcode_version = PROVISION_QR_MAX_VERSION;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    if (esp_qrcode_generate(&config, text) != ESP_OK) {
        return 0;
    }
    return job.written;
#else
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(PROVISION_QR_MAX_VERSION)];
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(PROVISION_QR_MAX_VERSION)];
    int modules;
    int scale;
    if (!qrcodegen_encodeText(text, temp, qrcode, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              PROVISION_QR_MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
        return 0;
    }
    modules = qrcodegen_getSize(qrcode);
    scale = PROVISION_QR_MAX_SIDE / (modules + PROVISION_QR_QUIET * 2);
    if (scale < 2) {
        return 0;
    }
    return provision_qoi_encode(modules, module_from_qrcodegen, qrcode, PROVISION_QR_QUIET, scale, out, cap);
#endif
}
