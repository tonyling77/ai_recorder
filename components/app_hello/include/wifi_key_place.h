#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_KEY_COUNT 39
#define WIFI_KEY_PARK_Y 1000

typedef struct {
    int16_t x;
    int16_t y;
} wifi_key_place_t;

wifi_key_place_t wifi_key_place(size_t index, bool shown);
