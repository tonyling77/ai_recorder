// SPDX-License-Identifier: Apache-2.0
/*
 * The wall clock of the board, without the RainMaker MQTT client.
 *
 * The RainMaker cloud client used to carry the time sync. It is off, because
 * its endless TLS reconnect loop drained the internal DMA pool and broke the
 * microphone. This module replaces that one clock source with a plain lwIP
 * SNTP client, so the board keeps a wall clock and the RainMaker MQTT client
 * stays out of the binary.
 */
#include "app_clock.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "app_clock";

/* The board sits in China. The POSIX sign is the offset west of Greenwich,
 * so UTC+8 is CST-8. */
#define APP_CLOCK_TZ "CST-8"

/* The epoch is what the clock reads before the first sync. A year below this
 * means the wall clock was never set. */
#define APP_CLOCK_MIN_YEAR 2024

/* pool.ntp.org resolves to a Chinese mirror as well, and the board is in
 * China, so the .cn pool is the one that answers fastest here. */
#define APP_CLOCK_SNTP_SERVER "cn.pool.ntp.org"

/* The board may join the Wi-Fi before the router or the internet is ready,
 * so the first attempt can fail. The client keeps retrying on the lwIP
 * interval, and this loop restarts it if the whole client died. */
#define APP_CLOCK_RETRY_MS 30000
#define APP_CLOCK_TASK_STACK 4096
#define APP_CLOCK_TASK_PRIO 4

static volatile bool s_synced;
static bool s_started;

static void log_clock(const char *prefix);

/* The lwIP notification runs in the tcpip task. It may be called before the
 * task that starts the client is done, so the state is flipped first and the
 * log line follows. */
static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    if (s_synced) {
        /* lwIP re-syncs on its own interval. The first one is the one the
         * user cares about, the later ones only refresh the drift. */
        return;
    }
    s_synced = true;
    log_clock("clock set");
}

static void apply_timezone(void)
{
    setenv("TZ", APP_CLOCK_TZ, 1);
    tzset();
}

static void log_clock(const char *prefix)
{
    time_t now = time(NULL);
    struct tm local;

    if (now < 0 || localtime_r(&now, &local) == NULL) {
        ESP_LOGW(TAG, "%s: wall clock is not readable", prefix);
        return;
    }
    ESP_LOGI(TAG, "%s: %04d-%02d-%02d %02d:%02d:%02d %s", prefix, local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, APP_CLOCK_TZ);
}

/* The SNTP client runs on its own task so a slow DNS lookup or an
 * unreachable server never holds the Wi-Fi join or the UI.
 *
 * The lwIP client retries the DNS lookup and the server on its own, so a
 * router that is not ready yet is not fatal. The loop below covers the
 * other case: the client itself could not be created. */
static void clock_task(void *arg)
{
    (void)arg;

    apply_timezone();
    log_clock("clock before sync");

    while (true) {
        /* The netif client sets the poll mode and the server name on the lwIP
         * SNTP client: it calls esp_sntp_setoperatingmode(SNTP_OPMODE_POLL)
         * and esp_sntp_setservername(0, server) before it starts the client. */
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_CLOCK_SNTP_SERVER);
        config.sync_cb = on_time_sync;
        /* The task does not wait for the sync: it keeps the retry loop alive
         * and reports the clock as soon as the callback fires. */
        config.wait_for_sync = false;
        config.start = true;

        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
            /* The board may have joined before the internet was reachable.
             * Wait and try again instead of leaving the device without a
             * clock. */
            ESP_LOGW(TAG, "SNTP not started; retrying in %d ms", (int)APP_CLOCK_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(APP_CLOCK_RETRY_MS));
            continue;
        }

        ESP_LOGI(TAG, "SNTP client started against %s", APP_CLOCK_SNTP_SERVER);
        while (!s_synced) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        /* The client stays up. lwIP re-syncs it on its own interval and
         * keeps the callback, so the clock survives a long sleep. */
        vTaskDelete(NULL);
        return;
    }
}

void app_clock_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    apply_timezone();
    if (xTaskCreate(clock_task, "app_clock", APP_CLOCK_TASK_STACK, NULL, APP_CLOCK_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "clock task create failed");
        s_started = false;
    }
}

bool app_clock_valid(void)
{
    time_t now = time(NULL);
    struct tm local;

    if (now < 0 || localtime_r(&now, &local) == NULL) {
        return false;
    }
    return local.tm_year + 1900 >= APP_CLOCK_MIN_YEAR;
}

void app_clock_log(const char *prefix)
{
    log_clock(prefix ? prefix : "clock");
}
