/* The recorder over the link to the host.
 *
 * The recording is started and stopped by the person in front of the
 * board, and the upload is asked for with the same finger. That makes the
 * whole path hard to check from a host, and hard to check at all when the
 * board is on a bench.
 *
 * This handler lets the host ask for the same things the buttons ask
 * for. It goes through the same request of the application, so it is the
 * same code and the same queue.
 *
 * The command is one byte of the payload. The answer says what the
 * recorder is doing now, so a host can tell whether the recording is
 * running and whether a write is on its way. */

#include "recorder_control_rpc.h"

#include <stdio.h>
#include <string.h>

#include "app_hello.h"
#include "esp_iris.h"
#include "esp_log.h"

static const char *TAG = "recorder_control";

/* The recorder of the application. It is set once, when the application
 * is open, and it stays for the life of the board. */
static app_hello_handle_t s_recorder;

static esp_err_t run_command(const esp_iris_rpc_request_t *request, uint8_t *response, size_t response_capacity,
                              size_t *response_size, void *user_ctx)
{
    app_hello_command_t command;
    app_hello_snapshot_t snapshot;
    char answer[96];
    int written;
    uint8_t wanted;

    (void)user_ctx;
    if (!request || !request->payload || request->payload_size < 1) {
        written = snprintf((char *)response, response_capacity, "error: the request carried no command");
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        return ESP_FAIL;
    }
    wanted = request->payload[0];
    if (wanted != RECORDER_CONTROL_COMMAND_TOGGLE_RECORD && wanted != RECORDER_CONTROL_COMMAND_UPLOAD &&
        wanted != RECORDER_CONTROL_COMMAND_STATE && wanted != RECORDER_CONTROL_COMMAND_CHANNEL) {
        written = snprintf((char *)response, response_capacity, "error: unknown command %u", (unsigned)wanted);
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        return ESP_FAIL;
    }
    if (!s_recorder) {
        written = snprintf((char *)response, response_capacity, "error: the recorder is not open");
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        return ESP_FAIL;
    }
    if (wanted == RECORDER_CONTROL_COMMAND_STATE) {
        /* The state alone, so a host can wait for the recorder to settle
         * without touching it. */
        goto report;
    }
    if (wanted == RECORDER_CONTROL_COMMAND_CHANNEL) {
        /* The channel is the first byte, the private inbox or the cloud. */
        uint8_t which;
        if (request->payload_size < 2) {
            written = snprintf((char *)response, response_capacity, "error: the request carried no channel");
            *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
            return ESP_FAIL;
        }
        which = request->payload[1];
        if (which > 1) {
            written = snprintf((char *)response, response_capacity, "error: unknown channel %u", (unsigned)which);
            *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
            return ESP_FAIL;
        }
        if (app_hello_switch_channel(s_recorder,
                                      (which == 1) ? APP_HELLO_CHANNEL_XIAOZHI : APP_HELLO_CHANNEL_PRIVATE) !=
            ESP_GSP_OK) {
            written = snprintf((char *)response, response_capacity, "error: the switch was refused");
            *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "the host asked for channel %u", (unsigned)which);
        goto report;
    }
    command = (wanted == RECORDER_CONTROL_COMMAND_TOGGLE_RECORD) ? APP_HELLO_COMMAND_TOGGLE_RECORD
                                                                 : APP_HELLO_COMMAND_UPLOAD;
    if (app_hello_request(s_recorder, command) != ESP_GSP_OK) {
        written = snprintf((char *)response, response_capacity, "error: the request was refused");
        *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
        return ESP_FAIL;
    }
    /* The state of the recorder, so the host can tell what happened. */
report:
    memset(&snapshot, 0, sizeof(snapshot));
    (void)app_hello_read(s_recorder, &snapshot);
    written = snprintf(answer, sizeof(answer), "ok recording=%d sessions=%u pending=%u notes=%u channel=%d work=%d",
                       (int)snapshot.recording, (unsigned)snapshot.session_count,
                       (unsigned)snapshot.pending_count, (unsigned)snapshot.notes_count,
                       (int)snapshot.channel, (int)snapshot.work_channel);
    if (written < 0 || (size_t)written >= response_capacity) {
        written = snprintf((char *)response, response_capacity, "ok");
    } else {
        memcpy(response, answer, (size_t)written);
    }
    *response_size = (written > 0 && (size_t)written < response_capacity) ? (size_t)written : 0;
    ESP_LOGI(TAG, "the host asked for command %u", (unsigned)wanted);
    return ESP_OK;
}

void recorder_control_rpc_set_recorder(app_hello_handle_t recorder)
{
    s_recorder = recorder;
}

void recorder_control_rpc_start(void)
{
    esp_err_t err = esp_iris_rpc_register(RECORDER_CONTROL_RPC_SERVICE_ID, RECORDER_CONTROL_RPC_METHOD_COMMAND,
                                           run_command, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the recorder handler could not be registered: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "the host may start and stop the recorder");
}
