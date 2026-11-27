#include "recorder_store_lock.h"

#include <stdlib.h>

#if defined(ESP_PLATFORM)

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "recorder_store_lock";

/* One recursive mutex for the whole /nand recordings tree. LittleFS has no
 * internal locking, so every reader and writer has to share this lock. */
static SemaphoreHandle_t s_lock;

void recorder_store_lock_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "cannot create the recordings lock");
    }
}

int recorder_store_lock_acquire(uint32_t timeout_ms)
{
    TickType_t ticks;
    if (!s_lock) {
        /* The volume was never prepared. Without the mutex the tree would
         * be read and written without any protection, so refuse instead. */
        return -1;
    }
    ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(s_lock, ticks) != pdTRUE) {
        return -1;
    }
    return 0;
}
void recorder_store_lock_release(void)
{
    if (s_lock) {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

#else

/* The host tests run the store on one thread, so the lock is a no-op there.
 * The firmware build is the one that needs the serialisation. */
void recorder_store_lock_init(void)
{
}

int recorder_store_lock_acquire(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}

void recorder_store_lock_release(void)
{
}

#endif
