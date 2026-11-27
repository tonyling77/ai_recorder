#include "ai_key_wifi.h"

void ai_key_wifi_reset(ai_key_wifi_t *key)
{
    if (!key) {
        return;
    }
    wifi_gesture_reset(&key->gesture);
    key->down = false;
    key->record_waiting = false;
    key->down_ms = 0;
    key->released_ms = 0;
}

ai_key_wifi_action_t ai_key_wifi_sample(ai_key_wifi_t *key, bool pressed, uint32_t time_ms)
{
    if (!key) {
        return AI_KEY_WIFI_NONE;
    }
    if (pressed) {
        if (!key->down) {
            key->down = true;
            key->down_ms = time_ms;
            key->record_waiting = false;
        }
        return AI_KEY_WIFI_NONE;
    }
    if (!key->down) {
        if (key->record_waiting && (time_ms - key->released_ms) >= AI_KEY_RECORD_GAP_MS) {
            key->record_waiting = false;
            wifi_gesture_reset(&key->gesture);
            return AI_KEY_WIFI_RECORD;
        }
        return AI_KEY_WIFI_NONE;
    }
    key->down = false;
    uint32_t held_ms = time_ms - key->down_ms;
    if (held_ms < AI_KEY_SHORT_MIN_MS || held_ms > AI_KEY_SHORT_MAX_MS) {
        return AI_KEY_WIFI_NONE;
    }
    if (wifi_gesture_note_press(&key->gesture, time_ms) == WIFI_GESTURE_CLEAR_CREDENTIALS) {
        key->record_waiting = false;
        return AI_KEY_WIFI_CLEAR;
    }
    key->record_waiting = key->gesture.count == 1;
    if (key->record_waiting) {
        key->released_ms = time_ms;
    }
    return AI_KEY_WIFI_NONE;
}
