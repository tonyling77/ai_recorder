#pragma once

#include "wifi_gesture.h"

#include <stdbool.h>
#include <stdint.h>

#define AI_KEY_SHORT_MIN_MS 40
#define AI_KEY_SHORT_MAX_MS 800

#define AI_KEY_RECORD_GAP_MS 700

typedef enum {
    AI_KEY_WIFI_NONE = 0,
    AI_KEY_WIFI_CLEAR,
    AI_KEY_WIFI_RECORD,
} ai_key_wifi_action_t;

typedef struct {
    wifi_gesture_t gesture;
    bool down;
    bool record_waiting;
    uint32_t down_ms;
    uint32_t released_ms;
} ai_key_wifi_t;

void ai_key_wifi_reset(ai_key_wifi_t *key);
ai_key_wifi_action_t ai_key_wifi_sample(ai_key_wifi_t *key, bool pressed, uint32_t time_ms);
