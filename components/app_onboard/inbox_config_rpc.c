/* Let the host point the board at an inbox.
 *
 * The address of the inbox is not known when the firmware is built, and the
 * address of a laptop changes when the lease runs out. The host sends the
 * address over the link that carried the update, and the board keeps it in
 * NVS, so it survives a restart and a firmware update.
 *
 * The request is the origin in text, and optionally the name of the machine
 * on the next line. The reply is the origin that is now in use, or a line
 * that starts with "error:" when the origin was refused. */

#include "inbox_config_rpc.h"

#include "recorder_inbox.h"

#include <stdio.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_log.h"

static const char *TAG = "inbox_config";

static esp_err_t set_origin(const esp_iris_rpc_request_t *request, uint8_t *response,
                             size_t response_capacity, size_t *response_size, void *user_ctx)
{
    char payload[RECORDER_INBOX_ORIGIN_MAX + RECORDER_INBOX_NAME_MAX + 2];
    char origin[RECORDER_INBOX_ORIGIN_MAX];
    char name[RECORDER_INBOX_NAME_MAX];
    char *separator;
    size_t length;
    int written;

    (void)user_ctx;
    if (!request || !request->payload || request->payload_size == 0) {
        written = snprintf((char *)response, response_capacity, "error: the request carried no address");
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        return ESP_FAIL;
    }
    length = request->payload_size < sizeof(payload) - 1 ? request->payload_size : sizeof(payload) - 1;
    memcpy(payload, request->payload, length);
    payload[length] = '\0';
    /* The host sends text; a trailing newline is not part of the address. */
    while (length > 0 && (payload[length - 1] == '\n' || payload[length - 1] == '\r')) {
        payload[--length] = '\0';
    }
    separator = strchr(payload, '\n');
    if (separator) {
        *separator = '\0';
        snprintf(name, sizeof(name), "%s", separator + 1);
    } else {
        name[0] = '\0';
    }
    if (recorder_inbox_set(payload, name) != 0) {
        written = snprintf((char *)response, response_capacity, "error: the address was refused");
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        ESP_LOGW(TAG, "the host sent an address that was refused");
        return ESP_FAIL;
    }
    recorder_inbox_get(origin, sizeof(origin), NULL, 0);
    ESP_LOGI(TAG, "the host pointed the board at %s", origin);
    written = snprintf((char *)response, response_capacity, "%s", origin);
    *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
    return ESP_OK;
}

void inbox_config_rpc_start(void)
{
    esp_err_t err = esp_iris_rpc_register(INBOX_CONFIG_RPC_SERVICE_ID, INBOX_CONFIG_RPC_METHOD_SET,
                                           set_origin, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the inbox address handler could not be registered: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "the host may point the board at an inbox");
}
