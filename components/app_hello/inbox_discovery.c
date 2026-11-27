/* Finding the inbox on the network.
 *
 * The inbox announces itself as _esp-inbox._tcp. The board browses for that
 * service type and keeps what it finds. The address that goes with a
 * service is the mDNS name of the machine, not its address, so what is
 * stored stays good when the address of the machine changes.
 *
 * A found inbox is not used on its own. The application asks the user, and
 * the answer is what makes it the address the device talks to. */

#include "inbox_discovery.h"

#include "recorder_inbox.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "inbox_discovery";

#define DISCOVERY_SERVICE "_esp-inbox"
#define DISCOVERY_PROTO "_tcp"

typedef struct {
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char name[RECORDER_INBOX_NAME_MAX];
    char host[64];
    bool confirmed;
    bool present;
} candidate_t;

static candidate_t s_candidates[INBOX_DISCOVERY_MAX];
static size_t s_count;
static bool s_started;

/* The browse callback runs in the mDNS task and the application reads the
 * list in its tick, so the list is under a lock. It is held for a copy of
 * a few strings, so neither task waits long for it. */
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

static void on_found(mdns_result_t *result)
{
    char hostname[64];
    char host[80];
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char name[RECORDER_INBOX_NAME_MAX];
    size_t length;

    if (!result || !result->hostname || !result->port) {
        return;
    }
    /* The record is only good while the callback runs. */
    length = strlen(result->hostname);
    if (length >= sizeof(hostname)) {
        length = sizeof(hostname) - 1;
    }
    memcpy(hostname, result->hostname, length);
    hostname[length] = '\0';
    while (length > 0 && hostname[length - 1] == '.') {
        hostname[--length] = '\0';
    }
    if (!hostname[0]) {
        return;
    }
    /* The mDNS component splits a name into its labels, so the domain is
     * not part of the host it reports. A service on the local network
     * lives in the local domain, and the board can only resolve a name
     * that says so. */
    if (strchr(hostname, '.')) {
        memcpy(host, hostname, length + 1);
    } else {
        memcpy(host, hostname, length);
        memcpy(host + length, ".local", sizeof(".local"));
    }
    snprintf(origin, sizeof(origin), "http://%s:%u", host, (unsigned)result->port);
    if (result->instance_name) {
        length = strlen(result->instance_name);
        if (length >= sizeof(name)) {
            length = sizeof(name) - 1;
        }
        memcpy(name, result->instance_name, length);
        name[length] = '\0';
    } else {
        length = strlen(host);
        if (length >= sizeof(name)) {
            length = sizeof(name) - 1;
        }
        memcpy(name, host, length);
        name[length] = '\0';
    }

    lock_take();
    /* A goodbye record carries a lifetime of zero: the service is gone. */
    if (result->ttl == 0) {
        for (size_t i = 0; i < s_count; i++) {
            if (strcmp(s_candidates[i].host, host) == 0) {
                s_candidates[i].present = false;
                ESP_LOGI(TAG, "the inbox %s is gone", hostname);
            }
        }
        lock_give();
        return;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_candidates[i].origin, origin) == 0) {
            s_candidates[i].present = true;
            lock_give();
            return;
        }
    }
    if (s_count >= INBOX_DISCOVERY_MAX) {
        ESP_LOGW(TAG, "there are more inboxes than the board keeps");
        lock_give();
        return;
    }
    snprintf(s_candidates[s_count].origin, sizeof(s_candidates[s_count].origin), "%s", origin);
    snprintf(s_candidates[s_count].name, sizeof(s_candidates[s_count].name), "%s", name);
    snprintf(s_candidates[s_count].host, sizeof(s_candidates[s_count].host), "%s", host);
    s_candidates[s_count].confirmed = false;
    s_candidates[s_count].present = true;
    s_count++;
    ESP_LOGI(TAG, "the inbox %s was found at %s", name, origin);
    lock_give();
}

void inbox_discovery_start(void)
{
    mdns_browse_t *browse;
    esp_err_t err;
    if (s_started) {
        return;
    }
    s_started = true;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    err = mdns_init();
    if (err != ESP_OK) {
        /* mDNS needs an interface, so this may only run once the station has
         * an address. The caller tries again until it works. */
        ESP_LOGE(TAG, "mDNS could not be started: %s", esp_err_to_name(err));
        s_started = false;
        return;
    }
    browse = mdns_browse_new(DISCOVERY_SERVICE, DISCOVERY_PROTO, on_found);
    if (!browse) {
        ESP_LOGE(TAG, "the inbox service could not be browsed for");
        return;
    }
    ESP_LOGI(TAG, "looking for an inbox on the network");
}

size_t inbox_discovery_count(void)
{
    size_t count;
    lock_take();
    count = s_count;
    lock_give();
    return count;
}

int inbox_discovery_get(size_t index, char *origin, size_t origin_cap, char *name, size_t name_cap)
{
    if (!origin || origin_cap == 0) {
        return -1;
    }
    lock_take();
    if (index >= s_count) {
        lock_give();
        return -1;
    }
    snprintf(origin, origin_cap, "%s", s_candidates[index].origin);
    if (name && name_cap) {
        snprintf(name, name_cap, "%s", s_candidates[index].name);
    }
    lock_give();
    return 0;
}

bool inbox_discovery_pending(void)
{
    bool pending = false;
    lock_take();
    for (size_t i = 0; i < s_count; i++) {
        if (s_candidates[i].present && !s_candidates[i].confirmed) {
            pending = true;
            break;
        }
    }
    lock_give();
    return pending;
}

int inbox_discovery_confirm(size_t index)
{
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char name[RECORDER_INBOX_NAME_MAX];

    lock_take();
    if (index >= s_count) {
        lock_give();
        return -1;
    }
    snprintf(origin, sizeof(origin), "%s", s_candidates[index].origin);
    snprintf(name, sizeof(name), "%s", s_candidates[index].name);
    lock_give();

    if (recorder_inbox_set(origin, name) != 0) {
        return -1;
    }
    lock_take();
    for (size_t i = 0; i < s_count; i++) {
        s_candidates[i].confirmed = (i == index);
    }
    lock_give();
    ESP_LOGI(TAG, "the inbox %s is now in use", name);
    return 0;
}
