#include "recorder_inbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#include "recorder_flash.h"

static const char *TAG = "recorder_inbox";

/* The learned address is its own namespace, so it survives an update of the
 * application and is only lost when the flash is erased. */
#define RECORDER_INBOX_NVS_NAMESPACE "inbox"
#define RECORDER_INBOX_NVS_ORIGIN "origin"
#define RECORDER_INBOX_NVS_NAME "name"
#endif

static char s_origin[RECORDER_INBOX_ORIGIN_MAX];
static char s_name[RECORDER_INBOX_NAME_MAX];

#if defined(ESP_PLATFORM)
/* The origin is read by the upload worker and by the notes sync, and it is
 * written by the task that learns it. A half-written origin would be a
 * broken URL for one request, so the copy is under a lock. */
static SemaphoreHandle_t s_lock;

static void lock_take(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void store_in_nvs(const char *origin, const char *name)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(RECORDER_INBOX_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the inbox namespace could not be opened: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(handle, RECORDER_INBOX_NVS_ORIGIN, origin);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, RECORDER_INBOX_NVS_NAME, name ? name : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the inbox origin could not be stored: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "the inbox origin is now %s", origin);
}

static void load_from_nvs(void)
{
    nvs_handle_t handle = 0;
    size_t length = sizeof(s_origin);
    esp_err_t err = nvs_open(RECORDER_INBOX_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_get_str(handle, RECORDER_INBOX_NVS_ORIGIN, s_origin, &length);
    if (err == ESP_OK) {
        length = sizeof(s_name);
        if (nvs_get_str(handle, RECORDER_INBOX_NVS_NAME, s_name, &length) != ESP_OK) {
            s_name[0] = '\0';
        }
    } else {
        s_origin[0] = '\0';
        s_name[0] = '\0';
    }
    nvs_close(handle);
}
#endif

void recorder_inbox_init(void)
{
#if defined(ESP_PLATFORM)
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    load_from_nvs();
    if (s_origin[0]) {
        ESP_LOGI(TAG, "the inbox origin is %s", s_origin);
    }
#endif
}

static int origin_is_usable(const char *origin)
{
    const char *authority;
    size_t length;
    if (!origin || !origin[0]) {
        return -1;
    }
    length = strlen(origin);
    if (length >= RECORDER_INBOX_ORIGIN_MAX) {
        return -1;
    }
    if (strncmp(origin, "https://", 8) == 0) {
        authority = origin + 8;
    } else if (strncmp(origin, "http://", 7) == 0) {
        authority = origin + 7;
    } else {
        return -1;
    }
    /* A base URL has only an authority after the scheme: no path, no query,
     * no fragment and no space. */
    if (strchr(authority, '/') || strchr(authority, '?') || strchr(authority, '#') ||
        strchr(authority, ' ')) {
        return -1;
    }
    return 0;
}

int recorder_inbox_get(char *origin, size_t origin_cap, char *name, size_t name_cap)
{
    if (!origin || origin_cap == 0) {
        return -1;
    }
#if defined(ESP_PLATFORM)
    lock_take();
#endif
    snprintf(origin, origin_cap, "%s", s_origin);
    if (name && name_cap) {
        snprintf(name, name_cap, "%s", s_name);
    }
#if defined(ESP_PLATFORM)
    lock_give();
#endif
    return 0;
}

#if defined(ESP_PLATFORM)
/* The write of the inbox address.
 *
 * The flash is written with the cache of the flash turned off, and the
 * stack of the render task is in the external memory, which is out of
 * reach while that cache is off. So the write is done on a task whose
 * stack is in the internal memory.
 *
 * The two strings travel in a block of their own, because the write
 * happens later and on another task. The block belongs to the helper: it
 * is alive while this function runs and it is freed afterwards. */
struct inbox_write {
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char name[RECORDER_INBOX_NAME_MAX];
};

static void write_inbox(void *arg)
{
    const struct inbox_write *request = arg;
    /* The block is still alive here, so the strings are read from it
     * directly. Nothing large is put on the stack of the task. */
    store_in_nvs(request->origin, request->name);
}
#endif

int recorder_inbox_set(const char *origin, const char *name)
{
    if (origin_is_usable(origin) != 0) {
        return -1;
    }
#if defined(ESP_PLATFORM)
    lock_take();
#endif
    snprintf(s_origin, sizeof(s_origin), "%s", origin);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
#if defined(ESP_PLATFORM)
    lock_give();
    {
        struct inbox_write *request = malloc(sizeof(*request));
        if (!request) {
            ESP_LOGE(TAG, "the inbox origin could not be prepared");
            return 0;
        }
        snprintf(request->origin, sizeof(request->origin), "%s", origin);
        snprintf(request->name, sizeof(request->name), "%s", name ? name : "");
        recorder_flash_write(write_inbox, request);
    }
#else
    (void)name;
#endif
    return 0;
}

#if defined(ESP_PLATFORM)
/* The erase of the address.
 *
 * The erase is a write of the flash, so it runs on the task of the helper
 * as well: the stack of the render task is in the external memory, and the
 * flash is written with the cache of the flash turned off. */
static void clear_inbox(void *arg)
{
    nvs_handle_t handle = 0;
    (void)arg;
    if (nvs_open(RECORDER_INBOX_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, RECORDER_INBOX_NVS_ORIGIN);
        nvs_erase_key(handle, RECORDER_INBOX_NVS_NAME);
        nvs_commit(handle);
        nvs_close(handle);
    }
}
#endif

void recorder_inbox_clear(void)
{
#if defined(ESP_PLATFORM)
    lock_take();
#endif
    s_origin[0] = '\0';
    s_name[0] = '\0';
#if defined(ESP_PLATFORM)
    lock_give();
    recorder_flash_write(clear_inbox, NULL);
#endif
}

bool recorder_inbox_known(void)
{
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    recorder_inbox_get(origin, sizeof(origin), NULL, 0);
    return origin[0] != '\0';
}
