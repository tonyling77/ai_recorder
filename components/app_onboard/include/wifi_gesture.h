#pragma once

#include <stdint.h>

#define WIFI_GESTURE_WINDOW_MS 3000
#define WIFI_GESTURE_PRESS_COUNT 5

typedef enum {
    WIFI_GESTURE_IDLE = 0,
    WIFI_GESTURE_CLEAR_CREDENTIALS,
} wifi_gesture_result_t;

typedef struct {
    uint32_t started_ms;
    uint8_t count;
} wifi_gesture_t;

void wifi_gesture_reset(wifi_gesture_t *gesture);
wifi_gesture_result_t wifi_gesture_note_press(wifi_gesture_t *gesture, uint32_t time_ms);
