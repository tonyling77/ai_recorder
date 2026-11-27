// SPDX-License-Identifier: Apache-2.0
/*
 * RainMaker Neo BLE provisioning, claim and recorder cloud contract.
 * No Neo OTA. fctry is initialised, never bulk-erased from here.
 */
#include "app_onboard.h"
#include "ai_key_wifi.h"
#include "app_clock.h"
#include "node_presence.h"
#include "recorder_node.h"
#include "recorder_store.h"
#include "wifi_scan_plan.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_network_neo.h"
#include "network_provisioning/manager.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_credentials.h"
#include "esp_rmaker_node.h"
#include "esp_rmaker_system_ctrl.h"
#include "esp_rmaker_event_loop.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_services.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "osal_event_loop.h"
#include "osal_storage.h"
#include "sdkconfig.h"

static const char *TAG = "app_onboard";
static SemaphoreHandle_t wifi_joined;
static volatile bool wifi_handoff;

/* The RainMaker cloud client is off while the private channel carries the
 * product. Its MQTT client keeps reconnecting to a broker this device does
 * not use, and every TLS handshake takes a hardware SHA context out of the
 * internal DMA pool. The microphone buffers need that pool, so the client has
 * to stay down: without it the I2S DMA allocation fails and recording stops
 * with "Microphone unavailable".
 *
 * Wi-Fi is unaffected. Station start, reconnect and credential erase live in
 * rmaker_app_network and never went through esp_rmaker_start(). Flip this
 * back to 1 to restore the cloud node, the shadow and the Home controls. */
#define RAINMAKER_CLOUD_ENABLED 0

extern esp_rmaker_error_t esp_rmaker_system_ctrl_clear_data_namespaces(void);
extern esp_rmaker_error_t esp_rmaker_state_schedule_report(bool report_all);

/* Overrides the weak symbol in rmaker_app_network. It goes false while the
 * on-screen Wi-Fi scan or join holds the radio. */
bool app_wifi_reconnect_enabled(void);

typedef struct {
    app_hello_handle_t recorder;
    recorder_command_guard_t guard;
    esp_rmaker_param_t *params[8];
    recorder_session_info_t sessions[10];
    char recordings_json[768];
} recorder_cloud_t;

static recorder_cloud_t cloud;
static bool cloud_values_ready;

/* Returns true when the station already has an address. */
static bool log_sta_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        ESP_LOGW(TAG, "STA has no IP yet");
        return false;
    }
    ESP_LOGI(TAG, "STA IP " IPSTR, IP2STR(&info.ip));
    return true;
}

static void on_rmaker_event(void *arg, osal_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != RMAKER_EVENT) {
        return;
    }
    if (id == RMAKER_EVENT_CLAIM_STARTED) {
        ESP_LOGI(TAG, "claim started");
    } else if (id == RMAKER_EVENT_CLAIM_SUCCESSFUL) {
        ESP_LOGI(TAG, "claim ok");
    } else if (id == RMAKER_EVENT_CLAIM_FAILED) {
        ESP_LOGE(TAG, "claim failed");
    } else if (id == RMAKER_EVENT_CORE_STARTED) {
        ESP_LOGI(TAG, "RainMaker core started");
    }
}

static void on_mqtt_event(void *arg, osal_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != RMAKER_COMMON_EVENT) {
        return;
    }
    if (id == RMAKER_MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected");
    } else if (id == RMAKER_MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

static void ensure_factory_nvs(void)
{
    const char *label = CONFIG_ESP_RMAKER_FACTORY_PARTITION_NAME;
    esp_err_t err = nvs_flash_init_partition(label);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "factory NVS %s is unavailable: %s; preserving it", label, esp_err_to_name(err));
    }
}

static void ensure_pop_random(void)
{
    const char *part = CONFIG_APP_NETWORK_FACTORY_NVS_PARTITION_NAME;
    const char *ns = CONFIG_APP_NETWORK_PROV_DATA_NAMESPACE;
    const char *key = CONFIG_APP_NETWORK_PROV_RANDOM_NVS_KEY;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(part, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open PoP namespace %s/%s: %s", part, ns, esp_err_to_name(err));
        return;
    }
    size_t len = 0;
    err = nvs_get_blob(handle, key, NULL, &len);
    if (err == ESP_OK && len >= 4) {
        nvs_close(handle);
        return;
    }
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    err = nvs_set_blob(handle, key, random_bytes, sizeof(random_bytes));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seed PoP random: %s", esp_err_to_name(err));
    }
}

static esp_rmaker_error_t reset_wifi_only(void)
{
    (void)app_network_reset_credentials();
    return ESP_RMAKER_OK;
}

/* Stop STA before erasing the saved hotspot. A running STA writes the old
 * config back to flash on reboot. */
static esp_rmaker_error_t stop_sta_and_clear_wifi(void)
{
    esp_err_t stopped = esp_wifi_stop();
    if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_INIT &&
        stopped != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Wi-Fi STA stop failed: %s", esp_err_to_name(stopped));
        return ESP_RMAKER_FAIL;
    }
    wifi_config_t empty = {0};
    if (esp_wifi_set_storage(WIFI_STORAGE_FLASH) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &empty) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi STA config clear failed");
        return ESP_RMAKER_FAIL;
    }
    if (app_network_reset_credentials() != OSAL_ERR_OK) {
        ESP_LOGE(TAG, "Wi-Fi credential reset failed");
        return ESP_RMAKER_FAIL;
    }
    return ESP_RMAKER_OK;
}

/* This image clears the saved Wi-Fi once, so the board can join another AP.
 * The flag lives outside the Wi-Fi namespace. fctry, claim and recordings stay. */
static void clear_wifi_once(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("recorder", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open recorder nvs: %s", esp_err_to_name(err));
        return;
    }
    uint8_t done = 0;
    err = nvs_get_u8(handle, "wifi_cleared", &done);
    if (err == ESP_OK && done == 1) {
        nvs_close(handle);
        return;
    }
    if (app_network_reset_credentials() != OSAL_ERR_OK) {
        ESP_LOGE(TAG, "Wi-Fi credential reset failed");
        nvs_close(handle);
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi credentials cleared; starting BLE provisioning");
    err = nvs_set_u8(handle, "wifi_cleared", 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remember wifi clear: %s", esp_err_to_name(err));
    }
}

/* One boot drops the claimed node id, certificate, key and MQTT host, plus
 * the Wi-Fi credentials that would skip BLE. PoP and recordings stay.
 * The next boot enters provisioning and assisted claiming. */
static void reset_claim_once(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("recorder", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open recorder nvs: %s", esp_err_to_name(err));
        return;
    }
    uint8_t done = 0;
    err = nvs_get_u8(handle, "claim_purged", &done);
    if (err == ESP_OK && done == 1) {
        nvs_close(handle);
        return;
    }
    if (app_network_reset_credentials() != OSAL_ERR_OK) {
        ESP_LOGE(TAG, "Wi-Fi credential reset failed");
        nvs_close(handle);
        return;
    }
    if (esp_rmaker_system_ctrl_clear_data_namespaces() != ESP_RMAKER_OK) {
        ESP_LOGW(TAG, "RainMaker data namespaces were not fully cleared");
    }
    if (esp_rmaker_credentials_erase_claim_data() != ESP_RMAKER_OK) {
        ESP_LOGE(TAG, "claim credential erase failed");
        nvs_close(handle);
        return;
    }
    ESP_LOGW(TAG, "Claim credentials erased; next boot uses assisted claiming");
    err = nvs_set_u8(handle, "claim_purged", 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remember claim purge: %s", esp_err_to_name(err));
        return;
    }
    esp_restart();
}

/* One short press toggles recording after a short pause. Five short presses
 * factory-reset RainMaker data, stop STA and erase the saved hotspot.
 * fctry, claim and recordings stay. A long hold is not a press. */
static void ai_key_task(void *arg)
{
    app_hello_handle_t recorder = arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_BUTTON_AI_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "AI key GPIO%d config failed", (int)BSP_BUTTON_AI_GPIO);
        vTaskDelete(NULL);
        return;
    }
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    ESP_LOGI(TAG, "AI key GPIO%d: one short press records, five within 3s factory reset",
             (int)BSP_BUTTON_AI_GPIO);
    while (true) {
        bool pressed = gpio_get_level(BSP_BUTTON_AI_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ai_key_wifi_action_t action = ai_key_wifi_sample(&key, pressed, now_ms);
        if (action == AI_KEY_WIFI_CLEAR) {
            ESP_LOGW(TAG, "AI key x5: factory reset");
            if (esp_rmaker_system_ctrl_factory_reset(0, 0, stop_sta_and_clear_wifi) != ESP_RMAKER_OK) {
                ESP_LOGE(TAG, "factory reset failed");
            }
        } else if (action == AI_KEY_WIFI_RECORD) {
            if (app_hello_request(recorder, APP_HELLO_COMMAND_TOGGLE_RECORD) != ESP_GSP_OK) {
                ESP_LOGE(TAG, "AI key record request failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void start_ai_key(app_hello_handle_t recorder)
{
    if (xTaskCreate(ai_key_task, "ai_key", 4096, recorder, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "AI key task create failed");
    }
}

static void on_network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == APP_NETWORK_EVENT && id == APP_NETWORK_EVENT_QR_DISPLAY && data) {
        if (app_hello_offer_provision(arg, (const char *)data) != ESP_GSP_OK) {
            ESP_LOGE(TAG, "provision QR was not shown");
        }
    } else if (base == NETWORK_PROV_EVENT && id == NETWORK_PROV_WIFI_CRED_SUCCESS) {
        if (app_hello_finish_provision(arg) != ESP_GSP_OK) {
            ESP_LOGE(TAG, "provision screen was not closed");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
            ESP_LOGW(TAG, "failed to disable Wi-Fi power save");
        }
        /* The wall clock needs a route, so the SNTP client starts only now
         * that the station has an address. The client retries on its own, so
         * a router that is not ready yet does not leave the device without a
         * clock. */
        app_clock_start();
        app_clock_log("wall clock at IP");
    }
}

static void add_name_only(esp_rmaker_device_t *device, const recorder_node_spec_t *spec)
{
    if (!recorder_param_allowed(ESP_RMAKER_DEF_NAME_PARAM_ID)) {
        ESP_LOGE(TAG, "Name is not in the recorder param list");
        return;
    }
    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM_ID, spec->node_name);
    if (name) {
        (void)esp_rmaker_param_add_ui_type(name, recorder_param_ui_type(ESP_RMAKER_DEF_NAME_PARAM_ID));
        esp_rmaker_device_add_param(device, name);
    }
}

static esp_rmaker_param_val_t initial_value(recorder_value_type_t type)
{
    if (type == RECORDER_VALUE_INT) {
        return esp_rmaker_int(0);
    }
    return esp_rmaker_str("");
}

static void report_status(recorder_state_t state, recorder_error_t error)
{
    (void)esp_rmaker_param_update(cloud.params[3], esp_rmaker_str(recorder_state_name(state)));
    (void)esp_rmaker_param_update_and_report(cloud.params[4], esp_rmaker_str(recorder_error_name(error)));
}

static recorder_state_t cloud_state(app_hello_state_t state)
{
    static const recorder_state_t states[] = {
        RECORDER_STATE_IDLE,
        RECORDER_STATE_RECORDING,
        RECORDER_STATE_UPLOADING,
        RECORDER_STATE_UPLOAD_OK,
        RECORDER_STATE_ERROR,
    };
    return state >= APP_HELLO_STATE_IDLE && state <= APP_HELLO_STATE_ERROR ? states[state] : RECORDER_STATE_ERROR;
}

static recorder_error_t cloud_error(app_hello_error_t error)
{
    static const recorder_error_t errors[] = {
        RECORDER_ERROR_NONE,
        RECORDER_ERROR_MIC,
        RECORDER_ERROR_STORAGE,
        RECORDER_ERROR_NETWORK,
        RECORDER_ERROR_BUSY,
    };
    return error >= APP_HELLO_ERROR_NONE && error <= APP_HELLO_ERROR_BUSY ? errors[error] : RECORDER_ERROR_CLOUD;
}

static void refresh_recordings(recorder_cloud_t *state)
{
    recorder_store_t *store = recorder_store_open("/nand/recordings");
    size_t count = 0;
    if (!store || recorder_store_list_sessions(store, state->sessions,
                                                sizeof(state->sessions) / sizeof(state->sessions[0]), &count) != 0) {
        recorder_store_close(store);
        return;
    }
    recorder_store_close(store);
    size_t used = 0;
    state->recordings_json[used++] = '[';
    for (size_t i = 0; i < count; i++) {
        const recorder_session_info_t *item = &state->sessions[i];
        int written = snprintf(state->recordings_json + used, sizeof(state->recordings_json) - used,
                               "%s{\"id\":%lu,\"duration_ms\":%lu,\"status\":\"%s\"}",
                               i == 0 ? "" : ",", (unsigned long)item->session_id,
                               (unsigned long)item->duration_ms, item->pending ? "pending" : "uploaded");
        if (written < 0 || (size_t)written >= sizeof(state->recordings_json) - used) {
            return;
        }
        used += (size_t)written;
    }
    if (used + 2 > sizeof(state->recordings_json)) {
        return;
    }
    state->recordings_json[used++] = ']';
    state->recordings_json[used] = '\0';
    (void)esp_rmaker_param_update(state->params[7], esp_rmaker_str(state->recordings_json));
}

static void on_recorder_snapshot(const app_hello_snapshot_t *snapshot, void *ctx)
{
    recorder_cloud_t *state = ctx;
    if (!snapshot || !state) {
        return;
    }
    if (!RAINMAKER_CLOUD_ENABLED) {
        /* No node, so no params and no shadow. The UI keeps its own state. */
        return;
    }
    if (!state->params[6]) {
        return;
    }
    (void)esp_rmaker_param_update(state->params[3], esp_rmaker_str(recorder_state_name(cloud_state(snapshot->state))));
    (void)esp_rmaker_param_update(state->params[4], esp_rmaker_str(recorder_error_name(cloud_error(snapshot->error))));
    (void)esp_rmaker_param_update(state->params[5], esp_rmaker_int((int)snapshot->session_count));
    refresh_recordings(state);
    /* The first snapshot runs before MQTT is up. Queueing a state report here
     * occupies the work queue that must finish cloud setup and setNodeConfig. */
    if (!cloud_values_ready) {
        (void)esp_rmaker_param_update(state->params[6], esp_rmaker_int((int)snapshot->pending_count));
        return;
    }
    (void)esp_rmaker_param_update_and_report(state->params[6], esp_rmaker_int((int)snapshot->pending_count));
}

static esp_rmaker_error_t recorder_write_cb(const esp_rmaker_device_t *device, const esp_rmaker_param_t *param,
                                            const esp_rmaker_param_val_t val, void *priv_data,
                                            esp_rmaker_write_ctx_t *ctx)
{
    (void)device;
    (void)ctx;
    recorder_cloud_t *state = priv_data;
    const char *id = esp_rmaker_param_get_id(param);
    if (!state || !id) {
        return ESP_RMAKER_INVALID_ARG;
    }
    recorder_command_kind_t kind;
    app_hello_command_t command;
    if (strcmp(id, "RecordCommand") == 0) {
        kind = RECORDER_COMMAND_RECORD;
        command = APP_HELLO_COMMAND_TOGGLE_RECORD;
    } else if (strcmp(id, "UploadCommand") == 0) {
        kind = RECORDER_COMMAND_UPLOAD;
        command = APP_HELLO_COMMAND_UPLOAD;
    } else {
        return ESP_RMAKER_INVALID_ARG;
    }
    uint64_t nonce = 0;
    bool accepted = false;
    if (val.type == RMAKER_VAL_TYPE_INTEGER && val.val.i > 0) {
        nonce = (uint64_t)val.val.i;
        accepted = recorder_command_accept(&state->guard, kind, nonce);
    } else if (val.type == RMAKER_VAL_TYPE_BOOLEAN && val.val.b) {
        accepted = recorder_command_accept_invoke(&state->guard, kind, &nonce);
    } else if (val.type == RMAKER_VAL_TYPE_STRING && val.val.s && strcmp(val.val.s, "invoke") == 0) {
        accepted = recorder_command_accept_invoke(&state->guard, kind, &nonce);
    }
    if (!accepted || nonce > 2147483647ULL) {
        return ESP_RMAKER_INVALID_ARG;
    }
    if (app_hello_request(state->recorder, command) != ESP_GSP_OK) {
        report_status(RECORDER_STATE_ERROR, RECORDER_ERROR_CLOUD);
        return ESP_RMAKER_FAIL;
    }
    /* A Home switch always sends true. Report 0 so the switch returns to off
     * and can be pressed again. An explicit nonce stays visible. */
    if (val.type != RMAKER_VAL_TYPE_INTEGER) {
        return esp_rmaker_param_update_and_report(param, esp_rmaker_int(0));
    }
    return esp_rmaker_param_update_and_report(param, esp_rmaker_int((int)nonce));
}

static bool add_recorder_params(esp_rmaker_device_t *device, const recorder_node_spec_t *spec)
{
    add_name_only(device, spec);
    for (size_t i = 1; i < spec->param_count; i++) {
        const recorder_param_spec_t *param_spec = recorder_param_spec_at(i);
        uint8_t properties = PROP_FLAG_READ | (param_spec->writable ? PROP_FLAG_WRITE : 0);
        if (i == 1 || i == 2) {
            properties |= PROP_FLAG_PERSIST;
        }
        esp_rmaker_param_val_t value = initial_value(param_spec->value_type);
        if (i == 3) {
            value = esp_rmaker_str(recorder_state_name(RECORDER_STATE_IDLE));
        } else if (i == 4) {
            value = esp_rmaker_str(recorder_error_name(RECORDER_ERROR_NONE));
        }
        const char *home_type = recorder_param_ui_type(param_spec->id);
        cloud.params[i] = esp_rmaker_param_create(param_spec->id, home_type, value, properties);
        if (cloud.params[i]) {
            (void)esp_rmaker_param_add_ui_type(cloud.params[i], home_type);
        }
        if (!cloud.params[i] || esp_rmaker_device_add_param(device, cloud.params[i]) != ESP_RMAKER_OK) {
            ESP_LOGE(TAG, "add recorder param %s failed", param_spec->id);
            return false;
        }
        if (i == 7) {
            (void)esp_rmaker_param_update(cloud.params[i], esp_rmaker_str("[]"));
        }
        if (i == 1 || i == 2) {
            esp_rmaker_param_val_t *stored = esp_rmaker_param_get_val(cloud.params[i]);
            if (stored && stored->type == RMAKER_VAL_TYPE_INTEGER && stored->val.i > 0) {
                const recorder_command_kind_t kind = i == 1 ? RECORDER_COMMAND_RECORD : RECORDER_COMMAND_UPLOAD;
                cloud.guard.last_nonce[kind] = (uint32_t)stored->val.i;
            }
        }
    }
    return true;
}

static void node_config_report_task(void *arg)
{
    (void)arg;
    /* The SDK publishes setNodeConfig and one full shadow, including online
     * and ncfg_ver, before the phone has subscribed to the named shadow.
     * The checksum then suppresses another identical config. Wait until that
     * subscription can exist, repeat online, then change the config once so
     * ncfg_ver moves past the phone's baseline. */
    vTaskDelay(pdMS_TO_TICKS(20000));
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1) {
            vTaskDelay(pdMS_TO_TICKS(15000));
        }
        node_presence_action_t action = node_presence_action(attempt);
        ESP_LOGI(TAG, "presence follow-up %d online=%d bump=%d", attempt,
                 action.report_online, action.bump_config);
        if (action.bump_config) {
            const esp_rmaker_node_t *node = esp_rmaker_get_node();
            if (!node || esp_rmaker_node_add_attribute(node, "cloud-sync", "1") != ESP_RMAKER_OK) {
                ESP_LOGW(TAG, "cloud-sync attribute was not added");
            } else if (esp_rmaker_report_node_config() != ESP_RMAKER_OK) {
                ESP_LOGW(TAG, "changed node config was not published");
            }
        }
        if (action.report_online && esp_rmaker_state_schedule_report(true) != ESP_RMAKER_OK) {
            ESP_LOGW(TAG, "online state was not scheduled");
        }
    }
    vTaskDelete(NULL);
}

static bool ensure_sta(void)
{
    esp_err_t start;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        return false;
    }
    start = esp_wifi_start();
    return start == ESP_OK || start == ESP_ERR_INVALID_STATE;
}

/* Reconnect the station after a drop.
 *
 * This mirrors the handler rmaker_app_network installs on its already
 * provisioned branch. app_wifi_reconnect_enabled() is the weak symbol the
 * app overrides; it goes false while the on-screen scan or join holds the
 * radio, and the reconnect has to wait for that hold to end. */
static void wifi_reconnect_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }
    if (!app_wifi_reconnect_enabled()) {
        return;
    }
    if (id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason is in the log, so a board that cannot hold the network
         * can be told apart from a board that is only slow. */
        const wifi_event_sta_disconnected_t *event = data;
        if (event) {
            ESP_LOGI(TAG, "Disconnected (reason %d). Connecting to the AP again...", (int)event->reason);
        } else {
            ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        }
        (void)esp_wifi_connect();
    }
}

/* Start the Wi-Fi station without the RainMaker provisioning bring-up.
 *
 * The provisioning flow calls esp_rmaker_pre_prov_init(), which starts the
 * RainMaker MQTT client. That client keeps reconnecting to a broker this
 * device does not use, and every TLS handshake takes a hardware SHA context
 * out of the internal DMA pool. The I2S microphone buffers need that pool,
 * so with the client running the recording fails with "Microphone
 * unavailable".
 *
 * This is the same work rmaker_app_network does on its already-provisioned
 * branch: register the reconnect handler and start the station. The
 * credentials are already in flash, so there is nothing to provision.
 */
static bool start_wifi_only(void)
{
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_reconnect_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi reconnect handler registration failed");
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi mode query failed");
        return false;
    }
    if (mode != WIFI_MODE_STA && esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi station mode failed");
        return false;
    }
    esp_err_t start = esp_wifi_start();
    if (start != ESP_OK && start != ESP_ERR_INVALID_STATE) {
        /* The Wi-Fi sheet may already have started the station. A station that
         * is up is a good outcome, so accept it instead of failing the boot. */
        wifi_mode_t running = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&running) != ESP_OK || running != WIFI_MODE_STA) {
            ESP_LOGE(TAG, "Wi-Fi station start failed: %s", esp_err_to_name(start));
            return false;
        }
        ESP_LOGI(TAG, "Wi-Fi station was already running");
    }
    return true;
}

static bool apply_sta_config(const char *ssid, const char *password)
{
    wifi_config_t chosen = {0};
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > sizeof(chosen.sta.ssid) || password_len >= sizeof(chosen.sta.password)) {
        return false;
    }
    memcpy(chosen.sta.ssid, ssid, ssid_len);
    memcpy(chosen.sta.password, password, password_len);
    chosen.sta.threshold.authmode = password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    chosen.sta.pmf_cfg.capable = true;
    return esp_wifi_set_config(WIFI_IF_STA, &chosen) == ESP_OK;
}

static bool save_sta_config(const char *ssid, const char *password)
{
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (apply_sta_config(ssid, password)) {
            return true;
        }
    }
    return false;
}

static bool join_selected_network(const char *ssid, const char *password)
{
    wifi_join_step_t steps[5];
    size_t count = wifi_join_steps(steps, sizeof(steps) / sizeof(steps[0]));
    bool paused = false;
    bool saved = false;
    if (count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        switch (steps[i]) {
        case WIFI_JOIN_PAUSE_RECONNECT:
            wifi_scan_hold_begin();
            paused = true;
            ESP_LOGW(TAG, "Wi-Fi join pausing reconnect");
            break;
        case WIFI_JOIN_DISCONNECT:
            (void)esp_wifi_disconnect();
            break;
        case WIFI_JOIN_SAVE:
            saved = save_sta_config(ssid, password);
            if (!saved) {
                ESP_LOGE(TAG, "Wi-Fi join could not save the password");
                if (paused) {
                    wifi_scan_hold_end();
                }
                return false;
            }
            break;
        case WIFI_JOIN_CONNECT:
            (void)esp_wifi_connect();
            break;
        case WIFI_JOIN_RESUME_RECONNECT:
            wifi_scan_hold_end();
            paused = false;
            break;
        }
    }
    return saved;
}

static bool station_is_reconnecting(void)
{
    wifi_ap_record_t ap = {0};
    wifi_config_t cfg = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0] != '\0') {
        return false;
    }
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

bool app_wifi_reconnect_enabled(void)
{
    return wifi_scan_reconnect_enabled();
}

static void scan_and_offer(app_hello_handle_t ui)
{
    wifi_scan_config_t scan = {0};
    wifi_ap_record_t aps[16];
    uint16_t count = 16;
    wifi_network_t networks[16] = {0};
    size_t found = 0;
    esp_err_t err = ESP_ERR_WIFI_STATE;
    bool resume = wifi_scan_should_disconnect(station_is_reconnecting());
    wifi_scan_hold_begin();
    if (resume) {
        ESP_LOGW(TAG, "Wi-Fi scan pausing reconnect");
        (void)esp_wifi_disconnect();
    }
    for (int attempt = 0; attempt < 5 && err == ESP_ERR_WIFI_STATE; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        count = 16;
        err = esp_wifi_scan_start(&scan, true);
    }
    if (err == ESP_OK && esp_wifi_scan_get_ap_records(&count, aps) == ESP_OK) {
        for (uint16_t i = 0; i < count && found < 16; i++) {
            memcpy(networks[found].ssid, aps[i].ssid, 32);
            networks[found].ssid[32] = '\0';
            networks[found].rssi = aps[i].rssi;
            networks[found].open = aps[i].authmode == WIFI_AUTH_OPEN;
            found++;
        }
    } else {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
    wifi_scan_hold_end();
    ESP_LOGI(TAG, "Wi-Fi scan found %u", (unsigned)found);
    if (resume) {
        (void)esp_wifi_connect();
    }
    (void)app_hello_offer_wifi(ui, networks, found);
}

static void wifi_sheet_task(void *arg)
{
    app_hello_handle_t ui = arg;
    while (true) {
        char ssid[33];
        char password[64];
        app_hello_wifi_action_t action = app_hello_poll_wifi(ui, ssid, sizeof(ssid), password, sizeof(password), 1000);
        if (action == APP_HELLO_WIFI_TIMEOUT) {
            continue;
        }
        if (!ensure_sta()) {
            ESP_LOGE(TAG, "Wi-Fi scan did not start");
            continue;
        }
        if (action == APP_HELLO_WIFI_RESCAN) {
            scan_and_offer(ui);
            continue;
        }
        if (!join_selected_network(ssid, password)) {
            continue;
        }
        ESP_LOGI(TAG, "Wi-Fi selected on device");
        if (!wifi_handoff) {
            wifi_handoff = true;
            esp_wifi_stop();
            if (wifi_joined) {
                (void)xSemaphoreGive(wifi_joined);
            }
        }
    }
}

static bool save_wifi_from_screen(app_hello_handle_t ui)
{
    wifi_config_t existing = {0};
    bool saved = esp_wifi_get_config(WIFI_IF_STA, &existing) == ESP_OK && existing.sta.ssid[0] != '\0';
    wifi_handoff = saved;
    wifi_joined = xSemaphoreCreateBinary();
    if (!wifi_joined || xTaskCreate(wifi_sheet_task, "wifi_sheet", 8192, ui, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi sheet did not start");
        return saved;
    }
    if (saved) {
        return true;
    }
    if (xSemaphoreTake(wifi_joined, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    return true;
}

static void onboard_task(void *arg)
{
    cloud.recorder = arg;
    recorder_node_spec_t spec;
    recorder_node_spec(&spec);

    /* Wi-Fi PHY and display timing warnings can overflow the retained log
     * before provisioning reaches MQTT and shadow reporting. */
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("present_te", ESP_LOG_ERROR);
    start_ai_key(cloud.recorder);
    osal_storage_init(NULL);
    ensure_factory_nvs();
    ensure_pop_random();
    app_network_init();
    clear_wifi_once();
    reset_claim_once();
    if (!save_wifi_from_screen(cloud.recorder)) {
        ESP_LOGE(TAG, "device Wi-Fi setup failed");
        vTaskDelete(NULL);
        return;
    }
    (void)esp_event_handler_register(APP_NETWORK_EVENT, APP_NETWORK_EVENT_QR_DISPLAY, on_network_event, cloud.recorder);
    (void)esp_event_handler_register(NETWORK_PROV_EVENT, NETWORK_PROV_WIFI_CRED_SUCCESS, on_network_event, cloud.recorder);
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_network_event, cloud.recorder);
    /* The Wi-Fi sheet may have joined the network before the handler above
     * was registered, so the address can already be there. The clock needs a
     * route, so it starts as soon as the station has one. */
    if (log_sta_ip()) {
        app_clock_start();
    }

    /* Neo Home only lists BLE devices that advertise the RMNG light or switch
     * filter. The published device type is esp.device.recorder.
     *
     * The provisioning bring-up starts the RainMaker MQTT client through
     * esp_rmaker_pre_prov_init(), so it has to sit behind the same switch as
     * the cloud node. With the client down the station still comes up, and
     * the credentials in flash are enough. */
    if (RAINMAKER_CLOUD_ENABLED) {
        if (app_network_provision(NEO_MFG_DATA_DEVICE_TYPE_LIGHT, NEO_MFG_DATA_DEVICE_SUBTYPE_LIGHT) != OSAL_ERR_OK) {
            ESP_LOGE(TAG, "Neo provisioning failed");
            vTaskDelete(NULL);
            return;
        }
    } else {
        if (!start_wifi_only()) {
            ESP_LOGE(TAG, "Wi-Fi station start failed");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "RainMaker cloud client disabled; Wi-Fi only");
    }
    if (log_sta_ip()) {
        app_clock_start();
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    /* The wall clock is not set yet at boot. The line makes that visible, so
     * a missing clock is never mistaken for a silent network problem. */
    app_clock_log("wall clock at boot");

    osal_err_t loop = osal_event_loop_create_default();
    if (loop != OSAL_ERR_OK && loop != OSAL_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %d", (int)loop);
        vTaskDelete(NULL);
        return;
    }
    (void)osal_event_handler_register(RMAKER_EVENT, RMAKER_EVENT_BASE_ANY, on_rmaker_event, NULL);

    if (RAINMAKER_CLOUD_ENABLED) {
        esp_rmaker_config_t config = {
            .enable_time_sync = true,
        };
        esp_rmaker_node_t *node = esp_rmaker_node_init(&config, spec.node_name, spec.node_type);
        if (!node) {
            ESP_LOGE(TAG, "node init failed");
            vTaskDelete(NULL);
            return;
        }
        esp_rmaker_device_t *device = esp_rmaker_device_create("Recorder", "esp.device.recorder", &cloud);
        if (!device) {
            ESP_LOGE(TAG, "device create failed");
            vTaskDelete(NULL);
            return;
        }
        if (!add_recorder_params(device, &spec)) {
            vTaskDelete(NULL);
            return;
        }
        (void)esp_rmaker_device_add_cb(device, recorder_write_cb, NULL);
        esp_rmaker_node_add_device(node, device);

        esp_rmaker_timezone_service_enable();
        esp_rmaker_system_serv_config_t system_service = {
            .flags = SYSTEM_SERV_FLAG_REBOOT | SYSTEM_SERV_FLAG_NETWORK_RESET,
            .reboot_seconds = 2,
            .reset_seconds = 2,
            .reset_reboot_seconds = 2,
            .network_reset_fn = reset_wifi_only,
        };
        esp_rmaker_system_service_enable(&system_service);
        (void)osal_event_handler_register(RMAKER_COMMON_EVENT, RMAKER_MQTT_EVENT_CONNECTED,
                                          on_mqtt_event, NULL);
        (void)osal_event_handler_register(RMAKER_COMMON_EVENT, RMAKER_MQTT_EVENT_DISCONNECTED,
                                          on_mqtt_event, NULL);
        esp_rmaker_start();
        ESP_LOGI(TAG, "recorder node started: %s (%s), params=%u", spec.node_name, spec.node_type,
                 (unsigned)spec.param_count);
        if (xTaskCreate(node_config_report_task, "rec_ncfg", 12288, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "node config report task was not created");
        }
    } else {
        /* No cloud client, so nothing reaches the RainMaker shadow. The
         * recorder still reports its state to the UI. */
        ESP_LOGI(TAG, "RainMaker node and shadow are not started");
    }

    (void)app_hello_set_observer(cloud.recorder, on_recorder_snapshot, &cloud);
    cloud_values_ready = true;
    vTaskDelete(NULL);
}

void app_onboard_start(app_hello_handle_t recorder)
{
    if (!recorder) {
        ESP_LOGE(TAG, "recorder controller is required");
        return;
    }
    if (xTaskCreate(onboard_task, "rec_onboard", 8192, recorder, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "onboard task create failed");
    }
}
