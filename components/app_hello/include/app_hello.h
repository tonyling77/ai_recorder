// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"
#include "wifi_picker.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque UI controller for AI Recorder. */
typedef struct app_hello *app_hello_handle_t;

typedef enum {
    APP_HELLO_COMMAND_TOGGLE_RECORD = 0,
    APP_HELLO_COMMAND_UPLOAD,
    APP_HELLO_COMMAND_CHANNEL,
} app_hello_command_t;

/* The channels the device can be on.
 *
 * The two channels are a choice: one of them is on and the other one is
 * off. The choice is kept, so the device comes back on it. */
typedef enum {
    APP_HELLO_CHANNEL_PRIVATE = 0,
    APP_HELLO_CHANNEL_XIAOZHI = 1,
} app_hello_channel_t;

typedef enum {
    APP_HELLO_STATE_IDLE = 0,
    APP_HELLO_STATE_RECORDING,
    APP_HELLO_STATE_UPLOADING,
    APP_HELLO_STATE_UPLOAD_OK,
    APP_HELLO_STATE_ERROR,
} app_hello_state_t;

typedef enum {
    APP_HELLO_ERROR_NONE = 0,
    APP_HELLO_ERROR_MIC,
    APP_HELLO_ERROR_STORAGE,
    APP_HELLO_ERROR_NETWORK,
    APP_HELLO_ERROR_BUSY,
} app_hello_error_t;

/** Value copy for diagnostics. */
typedef struct {
    uint32_t session_count;
    uint32_t pending_count;
    /* How many minutes the device holds. */
    uint32_t notes_count;
    uint32_t current_elapsed_s;
    bool recording;
    app_hello_state_t state;
    app_hello_error_t error;
    esp_gsp_err_t last_error;
    /* The channel the device is on. A host that switches the channel has to
     * be able to see that the switch happened. */
    app_hello_channel_t channel;
    /* The channel whose work is running.
     *
     * The choice of the user is on the screen at once, but the work of the
     * channel changes a moment later: the work of the channel that is left
     * has to end first, so the two channels never run at the same time. */
    app_hello_channel_t work_channel;
} app_hello_snapshot_t;

typedef void (*app_hello_observer_t)(const app_hello_snapshot_t *snapshot, void *ctx);

esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle);
esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot);
esp_gsp_err_t app_hello_offer_provision(app_hello_handle_t handle, const char *payload);
esp_gsp_err_t app_hello_finish_provision(app_hello_handle_t handle);
bool app_hello_provision_visible(app_hello_handle_t handle);
bool app_hello_provision_deferred(app_hello_handle_t handle);
typedef enum {
    APP_HELLO_WIFI_TIMEOUT = 0,
    APP_HELLO_WIFI_RESCAN,
    APP_HELLO_WIFI_JOIN,
} app_hello_wifi_action_t;

esp_gsp_err_t app_hello_offer_wifi(app_hello_handle_t handle, const wifi_network_t *networks, size_t count);
app_hello_wifi_action_t app_hello_poll_wifi(app_hello_handle_t handle, char *ssid, size_t ssid_cap,
                                            char *password, size_t password_cap, uint32_t timeout_ms);
esp_gsp_err_t app_hello_request(app_hello_handle_t handle, app_hello_command_t command);

/* Put the device on a channel, the way the settings page does.
 *
 * The switch releases what the channel that is left owns, so the two
 * channels never run at the same time.
 *
 * The switch is done by the task of the screen, like the other actions of
 * the interface. A caller on another task only asks for it; the screen is
 * not to be touched from two tasks at once. */
esp_gsp_err_t app_hello_switch_channel(app_hello_handle_t handle, app_hello_channel_t channel);
esp_gsp_err_t app_hello_set_observer(app_hello_handle_t handle, app_hello_observer_t observer, void *ctx);
esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle);

#ifdef __cplusplus
}
#endif
