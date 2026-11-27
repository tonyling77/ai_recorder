#include "wifi_gesture.h"

void wifi_gesture_reset(wifi_gesture_t *gesture)
{
    if (!gesture) {
        return;
    }
    gesture->started_ms = 0;
    gesture->count = 0;
}

wifi_gesture_result_t wifi_gesture_note_press(wifi_gesture_t *gesture, uint32_t time_ms)
{
    if (!gesture) {
        return WIFI_GESTURE_IDLE;
    }
    if (gesture->count == 0 || (time_ms - gesture->started_ms) > WIFI_GESTURE_WINDOW_MS) {
        gesture->started_ms = time_ms;
        gesture->count = 1;
        return WIFI_GESTURE_IDLE;
    }
    gesture->count++;
    if (gesture->count < WIFI_GESTURE_PRESS_COUNT) {
        return WIFI_GESTURE_IDLE;
    }
    gesture->count = 0;
    return WIFI_GESTURE_CLEAR_CREDENTIALS;
}
