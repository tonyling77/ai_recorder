#include "recorder_flash.h"

#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "recorder_flash";

/* The stack of the task that writes the flash.
 *
 * It has to be in the internal memory, and it has to hold the write and
 * the arguments of the write. The write of the flash is a few hundred
 * bytes deep, so this is enough with room to spare. */
#define RECORDER_FLASH_STACK 3072
#define RECORDER_FLASH_PRIO 4

static void flash_write_task(void *arg)
{
    struct {
        void (*write)(void *arg);
        void *arg;
    } *request = arg;
    void (*write)(void *arg) = request->write;
    void *value = request->arg;
    free(request);
    /* The name of the task is in the log, so it can be seen that the write
     * of the flash is done by this task and not by the task of the screen. */
    ESP_LOGI(TAG, "the flash is written by the task %s", pcTaskGetName(NULL));
    /* The write is a callback. The argument belongs to this function: it is
     * alive while the callback runs, and it is freed here, whether the
     * write worked or not. */
    write(value);
    free(value);
    vTaskDelete(NULL);
}

void recorder_flash_write(void (*write)(void *arg), void *arg)
{
    struct {
        void (*write)(void *arg);
        void *arg;
    } *request;
    if (!write) {
        return;
    }
    request = malloc(sizeof(*request));
    if (!request) {
        ESP_LOGE(TAG, "the write of the flash could not be prepared");
        return;
    }
    request->write = write;
    request->arg = arg;
    /* The task is created with the plain call, so its stack is in the
     * internal memory. That is the whole point of it. */
    if (xTaskCreate(flash_write_task, "flash_write", RECORDER_FLASH_STACK, request, RECORDER_FLASH_PRIO, NULL) != pdPASS) {
        /* No task will do the write, so the argument is freed here. */
        free(request->arg);
        free(request);
        ESP_LOGE(TAG, "the task of the write of the flash could not be created");
    }
}

#else

/* The desktop build has no flash and no scheduler for it. The write is done
 * right away, which keeps the order of the writes. */
void recorder_flash_write(void (*write)(void *arg), void *arg)
{
    if (write) {
        write(arg);
    }
}

#endif
