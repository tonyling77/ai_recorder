// SPDX-License-Identifier: Apache-2.0
#include "app_display.h"
#include "app_hello.h"
#include "app_onboard.h"
#include "inbox_config_rpc.h"
#include "recorder_control_rpc.h"
#include "recorder_inbox.h"
#include "recorder_volume.h"
#include "recorder_mic.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "recorder_font.h"
#include "ui_bundle.h"

static const char *TAG = "ai_recorder";

/* The composition root retains owned handles, never controller internals. */
static struct {
    esp_gsp_handle_t ui;
    app_hello_handle_t controller;
} application;

void app_main(void)
{
    ESP_LOGI(TAG, "the board starts with %u bytes of internal memory free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* The microphone is opened before anything else. The buffers of the
     * codec want one large block of the internal memory, and the heap can
     * only give that out before the screen and the Wi-Fi have taken their
     * share and broken it into pieces. */
    if (recorder_mic_prepare() != 0) {
        ESP_LOGE(TAG, "the microphone is not ready: %s", recorder_mic_failure());
    }
    ESP_LOGI(TAG, "the microphone is ready, %u bytes of internal memory free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* The address of the inbox is learned, not built in, so it has to be
     * read before the first request goes out. */
    recorder_inbox_init();
    /* The host may point the board at an inbox over the same link that
     * carried the update. */
    inbox_config_rpc_start();
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    recorder_font_apply(&config);
    /* The stack of the render task goes to the external memory.
     *
     * The screen is drawn with the Chinese font, so the render task needs
     * the bigger stack of the freetype path: forty kilobytes. They sat in the
     * internal memory, and the board has about three hundred and eighty
     * kilobytes of that. The buffers of the codec and the Wi-Fi need large
     * blocks of their own, and with the stack in the way there was not
     * enough left for the supplicant: the network component stopped the
     * board and it went into a reboot loop after every update.
     *
     * The external memory has plenty of room, and the stack is not the part
     * of the drawing that touches the flash for every pixel. */
    (void)esp_gsp_config_set(&config, ESP_GSP_FIELD_RENDER_TASK_STACK_PSRAM, 1);
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    if (recorder_volume_prepare() != 0) {
        ESP_LOGE(TAG, "recordings storage unavailable: %s", recorder_volume_failure());
    }
    ESP_LOGI(TAG, "the screen is up, %u bytes of memory free", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const esp_gsp_err_t ui_err = app_hello_create(application.ui, &application.controller);
    if (ui_err != ESP_GSP_OK) {
        ESP_LOGE(TAG, "recorder UI failed (0x%x); Recovery RPC remains active", ui_err);
        return;
    }
    /* The host can start and stop the recorder over the link that carries
     * the update, so the whole path can be checked without a person in
     * front of the board. */
    recorder_control_rpc_set_recorder(application.controller);
    recorder_control_rpc_start();
    /* GSP retains the callback context. Device UI/controller live until reboot;
     * teardown requires quiescing GSP before app_hello_destroy(), then closing UI. */
    ESP_LOGI(TAG, "GSP Hello World ready");
    /* BLE provisioning starts after the panel is up. Starting it earlier
     * exhausts the memory the display init needs and aborts app_main. */
    app_onboard_start(application.controller);
}
