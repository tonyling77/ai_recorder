#include "xiaozhi_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "uplink_channel.h"

#define XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

/* How long the stop waits for the check-in to end.
 *
 * The check-in ends when the request it is in returns, so the wait is as
 * long as that request can take. The request to the cloud has a timeout of
 * fifteen seconds, so the wait is a little longer than that.
 *
 * The wait is done by the task that switches the channel, not by the task
 * of the screen: the screen must not stand still while the other channel
 * lets go of what it holds. */
#define XIAOZHI_BIND_STOP_WAIT_MS 17000
#define XIAOZHI_BIND_STOP_POLL_MS 50

static const char *TAG = "xiaozhi";

static QueueHandle_t s_bodies;
static QueueHandle_t s_status;
static TaskHandle_t s_task;
static volatile bool s_stop;

static void mac_text(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void client_text(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, cap, "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

/* The activation reply carries credentials: the websocket token and the MQTT
 * username and password. Log the reply so the activation can be diagnosed,
 * with every credential value redacted. */
static void redact_value(char *copy, size_t *len, const char *key)
{
    char *found = strstr(copy, key);
    char *colon;
    char *start;
    char *end;

    if (!found) {
        return;
    }
    colon = strchr(found, ':');
    start = colon ? strchr(colon, '"') : NULL;
    if (!start) {
        return;
    }
    end = strchr(start + 1, '"');
    if (!end || (size_t)(end - copy) >= *len) {
        return;
    }
    /* The whole value is replaced, so no part of it is left in the buffer
     * that is logged. The tail moves back by the length the value loses. */
    {
        size_t value_len = (size_t)(end - start - 1);
        size_t tail_len = *len - (size_t)(end - copy);
        memmove(start + 4, end, tail_len + 1);
        memcpy(start + 1, "***", 3);
        *len = *len - value_len + 3;
        copy[*len] = '\0';
    }
}

static void log_reply(const char *what, const char *body)
{
    char copy[512];
    size_t len;

    if (!what) {
        what = "reply";
    }
    if (!body) {
        ESP_LOGI(TAG, "Xiaozhi %s: (empty)", what);
        return;
    }
    len = strlen(body);
    if (len >= sizeof(copy)) {
        len = sizeof(copy) - 1;
    }
    memcpy(copy, body, len);
    copy[len] = '\0';
    /* The redacted copy is a separate buffer, so the body that goes to the
     * parser keeps its original credentials. */
    redact_value(copy, &len, "\"token\"");
    redact_value(copy, &len, "\"username\"");
    redact_value(copy, &len, "\"password\"");
    ESP_LOGI(TAG, "Xiaozhi %s: %s", what, copy);
}

static int post_json(const char *url, const char *body, char *out, size_t cap, int *status)
{
    char device_id[24];
    char client_id[40];
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    int total = 0;
    if (!client) {
        return -1;
    }
    mac_text(device_id, sizeof(device_id));
    client_text(client_id, sizeof(client_id));
    esp_http_client_set_header(client, "Device-Id", device_id);
    esp_http_client_set_header(client, "Client-Id", client_id);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    esp_http_client_set_header(client, "User-Agent", "ai-recorder/1.0.0");
    /* The identity headers decide which device the server answers for, so
     * they are logged to make the check-in confirmable. */
    ESP_LOGI(TAG, "Xiaozhi request %s Device-Id %s Client-Id %s Activation-Version 1", url, device_id,
             client_id);
    if (esp_http_client_open(client, body ? (int)strlen(body) : 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    if (body && body[0] && esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    if (out && cap > 1) {
        while (total < (int)cap - 1) {
            int read = esp_http_client_read(client, out + total, cap - 1 - (size_t)total);
            if (read <= 0) {
                break;
            }
            total += read;
        }
        out[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return 0;
}

static void offer_body(char *body)
{
    if (!body) {
        return;
    }
    if (!s_bodies || xQueueSend(s_bodies, &body, 0) != pdTRUE) {
        free(body);
    }
}

static void offer_status(int status)
{
    if (s_status) {
        (void)xQueueOverwrite(s_status, &status);
    }
}

static void bind_task(void *arg)
{
    char mac[24];
    char uuid[40];
    char request[1024];
    xiaozhi_ota_identity_t id;
    (void)arg;
    ESP_LOGI(TAG, "Xiaozhi check-in started");
    mac_text(mac, sizeof(mac));
    client_text(uuid, sizeof(uuid));
    id = (xiaozhi_ota_identity_t){
        .mac = mac,
        .uuid = uuid,
        .chip = "esp32s31",
        .app_version = "1.0.0",
    };
    if (xiaozhi_ota_request(request, sizeof(request), &id) != 0) {
        s_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    while (!s_stop) {
        char *body = malloc(4096);
        int status = 0;
        xiaozhi_bind_view_t view;
        int parsed = -1;
        if (body && post_json(XIAOZHI_OTA_URL, request, body, 4096, &status) == 0 && status == 200) {
            parsed = xiaozhi_bind_view_from_ota(body, &view);
            ESP_LOGI(TAG, "Xiaozhi OTA status %d code %d", status, parsed == 0 && view.show_code);
            log_reply("OTA reply", body);
            offer_body(body);
            body = NULL;
        } else {
            ESP_LOGW(TAG, "Xiaozhi OTA status %d internal %u dma %u", status,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        }
        free(body);
        if (s_stop) {
            break;
        }
        if (parsed == 0 && !view.show_code) {
            break;
        }
        if (parsed != 0 || !view.show_code) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        for (int round = 0; round < 10 && !s_stop; round++) {
            int activate = 0;
            char scratch[64];
            int wait_ms = 10000;
            if (post_json(XIAOZHI_OTA_URL "activate", "{}", scratch, sizeof(scratch), &activate) == 0) {
                offer_status(activate);
                ESP_LOGI(TAG, "Xiaozhi activate status %d", activate);
                log_reply("activate reply", scratch);
                if (xiaozhi_bind_from_http(activate) == 1) {
                    s_stop = true;
                    break;
                }
                if (xiaozhi_bind_from_http(activate) == 0) {
                    wait_ms = 3000;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

void xiaozhi_bind_start(void)
{
    eTaskState state;
    if (!s_bodies) {
        s_bodies = xQueueCreate(1, sizeof(char *));
    }
    if (!s_status) {
        s_status = xQueueCreate(1, sizeof(int));
    }
    if (!s_bodies || !s_status) {
        return;
    }
    if (s_task) {
        state = eTaskGetState(s_task);
        if (state != eDeleted && state != eInvalid) {
            /* A check-in is still running. The flag of the stop is left
             * alone, because the running check-in has to end: a new one
             * would ask the cloud for the device a second time. */
            return;
        }
        s_task = NULL;
    }
    /* The flag is only cleared for a check-in that is really started. */
    s_stop = false;
    if (xTaskCreateWithCaps(bind_task, "xiaozhi_bind", 16384, NULL, 4, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "Xiaozhi task was not created");
    }
}

void xiaozhi_bind_stop(void)
{
    if (!s_task) {
        s_stop = true;
        return;
    }
    /* The check-in is asked to end and the caller waits for it, so the
     * memory of the check-in is free before the other channel starts its
     * work. The board does not have the memory for both channels at once. */
    s_stop = true;
    for (int waited = 0; waited < XIAOZHI_BIND_STOP_WAIT_MS && s_task; waited += XIAOZHI_BIND_STOP_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_BIND_STOP_POLL_MS));
    }
    if (s_task) {
        ESP_LOGW(TAG, "the check-in did not end in time; it will end by itself");
    }
}

char *xiaozhi_bind_take_body(void)
{
    char *body = NULL;
    if (!s_bodies || xQueueReceive(s_bodies, &body, 0) != pdTRUE) {
        return NULL;
    }
    return body;
}

int xiaozhi_bind_take_status(int *status)
{
    if (!status || !s_status || xQueueReceive(s_status, status, 0) != pdTRUE) {
        return 0;
    }
    return 1;
}
