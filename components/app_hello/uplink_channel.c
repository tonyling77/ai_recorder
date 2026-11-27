#include "uplink_channel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "nvs_flash.h"

#include "recorder_flash.h"

/* The channel is kept in the namespace the recorder already uses, so the
 * settings of the device live in one place. */
#define UPLINK_CHANNEL_NVS_NAMESPACE "recorder"
#define UPLINK_CHANNEL_NVS_KEY "channel"
#endif

uplink_channel_t uplink_channel_default(void)
{
    return UPLINK_CHANNEL_PRIVATE;
}

bool uplink_channel_runs_private(uplink_channel_t channel)
{
    return channel == UPLINK_CHANNEL_PRIVATE;
}

bool uplink_channel_runs_xiaozhi(uplink_channel_t channel)
{
    return channel == UPLINK_CHANNEL_XIAOZHI;
}

/* The name of the channel, as it is shown on the screen. */
const char *uplink_channel_label(uplink_channel_t channel)
{
    if (uplink_channel_runs_xiaozhi(channel)) {
        return "小智";
    }
    return "私有";
}

void uplink_channel_load(uplink_channel_t *out)
{
    if (!out) {
        return;
    }
    *out = uplink_channel_default();
#if defined(ESP_PLATFORM)
    nvs_handle_t handle = 0;
    uint8_t value = 0;
    if (nvs_open(UPLINK_CHANNEL_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if (nvs_get_u8(handle, UPLINK_CHANNEL_NVS_KEY, &value) == ESP_OK &&
        (value == UPLINK_CHANNEL_PRIVATE || value == UPLINK_CHANNEL_XIAOZHI)) {
        *out = (uplink_channel_t)value;
    } else {
        ESP_LOGI("uplink_channel", "no channel was stored yet");
    }
    nvs_close(handle);
#endif
}

#if defined(ESP_PLATFORM)
/* The write of the channel.
 *
 * The flash is written with the cache of the flash turned off, and the
 * stack of the render task is in the external memory, which is out of
 * reach while that cache is off. So the write is done on a task whose
 * stack is in the internal memory.
 *
 * The value travels in a small block of its own, because the write
 * happens later and on another task. The block belongs to the helper: it
 * is alive while this function runs and it is freed afterwards. */
static void write_channel(void *arg)
{
    uplink_channel_t channel = *(const uplink_channel_t *)arg;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(UPLINK_CHANNEL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("uplink_channel", "the channel could not be stored: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, UPLINK_CHANNEL_NVS_KEY, (uint8_t)channel);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE("uplink_channel", "the channel could not be stored: %s", esp_err_to_name(err));
    }
}
#endif

int uplink_channel_store(uplink_channel_t channel)
{
    if (channel != UPLINK_CHANNEL_PRIVATE && channel != UPLINK_CHANNEL_XIAOZHI) {
        return -1;
    }
#if defined(ESP_PLATFORM)
    {
        uplink_channel_t *value = malloc(sizeof(*value));
        if (!value) {
            ESP_LOGE("uplink_channel", "the channel could not be prepared");
            return -1;
        }
        *value = channel;
        recorder_flash_write(write_channel, value);
    }
#else
    (void)channel;
#endif
    return 0;
}

static const char *activation_code(const char *json)
{
    const char *activation = strstr(json, "\"activation\"");
    const char *code;
    size_t i;
    if (!activation) {
        return NULL;
    }
    code = strstr(activation, "\"code\"");
    if (!code) {
        return NULL;
    }
    code = strchr(code, ':');
    if (!code) {
        return NULL;
    }
    code = strchr(code, '"');
    if (!code) {
        return NULL;
    }
    code++;
    for (i = 0; i < 6; i++) {
        if (code[i] < '0' || code[i] > '9') {
            return NULL;
        }
    }
    if (code[6] != '"') {
        return NULL;
    }
    return code;
}

int xiaozhi_ota_request(char *out, size_t cap, const xiaozhi_ota_identity_t *id)
{
    int wrote;
    if (!out || cap < 64 || !id || !id->mac || !id->uuid || !id->chip || !id->app_version) {
        return -1;
    }
    wrote = snprintf(out, cap,
                     "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":16777216,"
                     "\"minimum_free_heap_size\":200000,\"mac_address\":\"%s\",\"uuid\":\"%s\","
                     "\"chip_model_name\":\"%s\",\"chip_info\":{\"model\":9,\"cores\":2,\"revision\":2,"
                     "\"features\":18},\"application\":{\"name\":\"ai_recorder\",\"version\":\"%s\"},"
                     "\"ota\":{\"label\":\"ota_0\"},\"board\":{\"type\":\"ai-recorder\","
                     "\"name\":\"ai-recorder\",\"mac\":\"%s\"}}",
                     id->mac, id->uuid, id->chip, id->app_version, id->mac);
    return wrote > 0 && (size_t)wrote < cap ? 0 : -1;
}

void xiaozhi_bind_view_waiting(xiaozhi_bind_view_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->hint, sizeof(out->hint), "%s", "正在获取验证码");
}

int xiaozhi_bind_view_from_ota(const char *json, xiaozhi_bind_view_t *out)
{
    const char *code;
    if (!json || !out || json[0] == '\0') {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    code = activation_code(json);
    if (!code) {
        if (!strstr(json, "\"activation\"") &&
            (strstr(json, "\"websocket\"") || strstr(json, "\"mqtt\"") || strstr(json, "\"firmware\""))) {
            snprintf(out->hint, sizeof(out->hint), "%s", "已绑定");
            return 0;
        }
        return -1;
    }
    memcpy(out->code, code, 6);
    out->code[6] = '\0';
    snprintf(out->hint, sizeof(out->hint), "%s", "登录 xiaozhi.me 后输入验证码");
    out->show_code = true;
    return 0;
}

int xiaozhi_bind_from_http(int status)
{
    if (status == 200) {
        return 1;
    }
    if (status == 202) {
        return 0;
    }
    return -1;
}

int xiaozhi_bind_already_bound(const char *json)
{
    xiaozhi_bind_view_t view;
    if (xiaozhi_bind_view_from_ota(json, &view) != 0) {
        /* A reply that cannot be read is not evidence of a bound device. */
        return 0;
    }
    return view.show_code ? 0 : 1;
}

/* The OTA reply may carry websocket.version. It decides the binary audio
 * frame layout, so a missing value means the original version 1. */
static int websocket_version(const char *socket, int fallback)
{
    const char *value = strstr(socket, "\"version\"");
    int version;
    if (!value) {
        return fallback;
    }
    value = strchr(value, ':');
    if (!value) {
        return fallback;
    }
    value++;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }
    if (*value < '0' || *value > '9') {
        return fallback;
    }
    version = 0;
    while (*value >= '0' && *value <= '9') {
        version = version * 10 + (*value - '0');
        value++;
    }
    if (version < 1 || version > 3) {
        return fallback;
    }
    return version;
}

int xiaozhi_bind_websocket(const char *json, char *url, size_t url_cap, char *token, size_t token_cap,
                           int *version)
{
    const char *socket;
    const char *value;
    const char *end;
    size_t length;
    if (!json || !url || url_cap < 8 || !token || token_cap < 2) {
        return -1;
    }
    if (version) {
        *version = 1;
    }
    socket = strstr(json, "\"websocket\"");
    if (!socket) {
        return -1;
    }
    value = strstr(socket, "\"url\"");
    if (!value) {
        return -1;
    }
    value = strchr(value, ':');
    value = value ? strchr(value, '"') : NULL;
    if (!value) {
        return -1;
    }
    value++;
    end = strchr(value, '"');
    if (!end || strncmp(value, "wss://", 6) != 0) {
        return -1;
    }
    length = (size_t)(end - value);
    if (length >= url_cap) {
        return -1;
    }
    memcpy(url, value, length);
    url[length] = '\0';
    value = strstr(socket, "\"token\"");
    if (!value) {
        return -1;
    }
    value = strchr(value, ':');
    value = value ? strchr(value, '"') : NULL;
    if (!value) {
        return -1;
    }
    value++;
    end = strchr(value, '"');
    if (!end) {
        return -1;
    }
    length = (size_t)(end - value);
    if (length == 0 || length >= token_cap) {
        return -1;
    }
    memcpy(token, value, length);
    token[length] = '\0';
    if (version) {
        *version = websocket_version(socket, *version);
    }
    return 0;
}
