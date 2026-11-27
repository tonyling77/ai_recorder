#include "recorder_mic.h"

#include "recorder_control.h"
#include "recorder_encoder.h"
#include "recorder_session.h"
#include "recorder_store_lock.h"
#include "recorder_volume.h"

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>

#define RECORDER_MIC_ROOT "/nand/recordings"
/* The stack of the task that reads the microphone.
 *
 * The encoder of the Opus codec puts arrays on the stack that are as long
 * as the frame it encodes: a 60 ms frame needs about eighteen kilobytes,
 * and the hardware stack guard ends the task before the array is even
 * filled. The stack is therefore in the external memory, where there is
 * plenty of it.
 *
 * The samples themselves are not on the stack: the sound card writes them
 * with direct memory access, so they have to be in the internal memory. */
#define RECORDER_MIC_STACK 40960
/* How much the microphone is read at a time.
 *
 * This is the grain of the read, not the length of the Opus frame. The
 * two are independent: the encoder gathers frames out of whatever comes
 * from the microphone.
 *
 * The read cannot be bigger than the buffer of the sound card: the driver
 * refuses a read that is bigger, and the read then times out after a
 * second. A read of 960 samples is 1920 bytes, and the buffer of the card
 * holds 1280 bytes, so the recording did not run at all: the board sat in
 * the read, the card filled up and threw the samples away, and the task
 * never got to the encoder.
 *
 * So the read stays at 320 samples, which is 20 ms of audio. */
#define RECORDER_MIC_CHUNK 320

static const char *TAG = "recorder_mic";
static const char *s_failure = "Cannot start recording";
static recorder_control_t *s_control;
static esp_codec_dev_handle_t s_mic;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile bool s_run;
static volatile int s_fault;
/* The elapsed time of the recording, published by the microphone task.
 *
 * The task holds the lock while it writes the audio to the flash, and that
 * write is the slow part. The screen reads the elapsed time every second
 * and it must not wait for that lock: it used to give up after 20 ms and
 * show 00:00 for the whole recording. */
static volatile uint32_t s_elapsed_ms;
/* When the recording started, in milliseconds since boot. The wall clock and
 * the recorded audio are compared when the recording stops: when they differ,
 * the audio was lost and the recording is shorter than the meeting. */
static int64_t s_started_ms;

static void mic_task(void *arg)
{
    /* The samples the sound card writes.
     *
     * The card writes them with direct memory access, so they have to be
     * in the internal memory, and they are not on the stack of the task:
     * the stack of the task is in the external memory, where the encoder
     * has the room it needs. */
    int16_t *pcm = heap_caps_malloc(RECORDER_MIC_CHUNK * sizeof(int16_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    (void)arg;
    if (!pcm) {
        ESP_LOGE(TAG, "the buffer of the microphone could not be made");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        int fed = 0;
        uint32_t frames = 0;
        /* The task waits for a recording. It used to end when the recording
         * ended, but it is created when the board starts, so it ended right
         * away and every press of the button had to create it again. */
        if (!s_run || s_fault || !s_control || !recorder_control_recording(s_control)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (esp_codec_dev_read(s_mic, pcm, (int)(RECORDER_MIC_CHUNK * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            uint32_t elapsed = 0;
            if (recorder_control_recording(s_control)) {
                fed = recorder_control_feed(s_control, pcm, RECORDER_MIC_CHUNK);
            }
            /* The elapsed time is published while the lock is held, so the
             * screen can read it without waiting for the write to the
             * flash. */
            if (recorder_control_elapsed_ms(s_control, &elapsed) == 0) {
                s_elapsed_ms = elapsed;
            }
            xSemaphoreGive(s_lock);
        }
        if (fed != 0) {
            s_fault = 1;
            s_failure = "Recording stopped";
        }
        frames++;
        if (frames % 50 == 0) {
            /* How much of the stack the encoding used, and how much memory
             * is left. The encoder reaches deeper with a longer frame, so
             * the number is in the log before the stack is too small. */
            ESP_LOGI(TAG, "the encoding used %u bytes of the %u byte stack, %u bytes of memory free",
                     (unsigned)(RECORDER_MIC_STACK - uxTaskGetStackHighWaterMark(NULL)), (unsigned)RECORDER_MIC_STACK,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static int ensure_microphone(void)
{
    esp_codec_dev_sample_info_t info = {
        .sample_rate = RECORDER_CAPTURE_HZ,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    if (s_mic) {
        return 0;
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "microphone init failed");
        return -1;
    }
    if (esp_codec_dev_open(s_mic, &info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed");
        return -1;
    }
    (void)esp_codec_dev_set_in_gain(s_mic, 30.0f);
    return 0;
}

int recorder_mic_prepare(void)
{
    /* Opening the microphone writes the registers of the codec over the slow
     * bus and ramps the gain, and that takes long enough that the button
     * feels stuck the first time. The work is the same every time, so it is
     * done once when the board starts, and the first press of Record only
     * has to open the file.
     *
     * The microphone stays open from then on. It uses a little more power
     * than a microphone that is opened for the recording, which is the price
     * of a button that answers at once. */
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    if (recorder_volume_prepare() != 0) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    if (!s_control) {
        s_control = recorder_control_open(RECORDER_MIC_ROOT);
    }
    if (!s_control || ensure_microphone() != 0) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    /* The stack of the task is in the external memory: the encoder of the
     * Opus codec needs a long stack for a 60 ms frame, and the internal
     * memory of the board is too small to give it one there. */
    if (!s_task && xTaskCreateWithCaps(mic_task, "rec_mic", RECORDER_MIC_STACK, NULL, 5, &s_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "the microphone task could not be created");
        return -1;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "the microphone is ready");
    return 0;
}

int recorder_mic_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        s_failure = "Out of memory";
        return -1;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        s_failure = "Cannot start recording";
        return -1;
    }
    if (!s_control) {
        s_control = recorder_control_open(RECORDER_MIC_ROOT);
    }
    if (recorder_volume_prepare() != 0) {
        s_failure = recorder_volume_failure();
    } else if (!s_control || ensure_microphone() != 0) {
        s_failure = "Microphone unavailable";
    } else if (recorder_control_start(s_control) != 0) {
        s_failure = "Cannot open recording";
    } else {
        s_failure = "";
        s_fault = 0;
        s_run = true;
        s_elapsed_ms = 0;
        s_started_ms = esp_timer_get_time();
        if (!s_task) {
            (void)recorder_control_stop(s_control);
            xSemaphoreGive(s_lock);
            s_failure = "The microphone is not ready";
            ESP_LOGE(TAG, "the microphone task is not running");
            return -1;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "recording started, %u bytes of memory free", (unsigned)esp_get_free_heap_size());
        return 0;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "record start failed: %s", s_failure);
    return -1;
}

const char *recorder_mic_failure(void)
{
    return s_failure;
}

int recorder_mic_fault(void)
{
    return s_fault;
}

static void log_saved(uint32_t session_id)
{
    for (uint16_t index = 0; index < RECORDER_SESSION_MAX_SLICES; index++) {
        char sha[65];
        char path[96];
        struct stat st;
        int missing;
        long size;
        if (recorder_control_sha256(s_control, session_id, (uint16_t)index, sha) != 0) {
            break;
        }
        snprintf(path, sizeof(path), "%s/%" PRIu32 "/%u.ogg", RECORDER_MIC_ROOT, session_id, (unsigned)index);
        /* The stat reads the shared tree, so it runs under the store lock. */
        if (recorder_store_lock_acquire(RECORDER_STORE_LOCK_TIMEOUT_MS) != 0) {
            break;
        }
        missing = stat(path, &st) != 0;
        size = missing ? 0 : (long)st.st_size;
        recorder_store_lock_release();
        if (missing) {
            ESP_LOGE(TAG, "saved session=%" PRIu32 " index=%u missing", session_id, (unsigned)index);
            break;
        }
        ESP_LOGI(TAG, "saved session=%" PRIu32 " index=%u bytes=%ld sha256=%s", session_id, (unsigned)index,
                 size, sha);
    }
}

int recorder_mic_stop(void)
{
    int stopped;
    uint32_t session_id;
    int64_t started_us;
    uint32_t recorded_ms;
    if (!s_lock || !s_control) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    started_us = esp_timer_get_time();
    session_id = recorder_control_last_session(s_control);
    recorded_ms = s_elapsed_ms;
    s_fault = 0;
    stopped = recorder_control_stop(s_control);
    if (stopped == 0) {
        log_saved(session_id);
    }
    xSemaphoreGive(s_lock);
    /* How long the recording took, and how long the audio is.
     *
     * When the two differ, the microphone did not keep up with the meeting
     * and the audio has a gap in it. The write to the flash is part of the
     * time here, so the numbers are read before it. */
    ESP_LOGI(TAG, "recording stopped: %u ms of audio in %lld ms, the write took %lld ms", (unsigned)recorded_ms,
             (long long)((started_us - s_started_ms) / 1000),
             (long long)((esp_timer_get_time() - started_us) / 1000));
    if (stopped != 0) {
        ESP_LOGE(TAG, "record stop failed");
    }
    ESP_LOGI(TAG, "the board has %u bytes of memory free", (unsigned)esp_get_free_heap_size());
    return stopped;
}

uint32_t recorder_mic_elapsed_ms(void)
{
    /* The value is published by the microphone task, so reading it does not
     * wait for the lock that the task holds while it writes to the flash. */
    return s_elapsed_ms;
}
