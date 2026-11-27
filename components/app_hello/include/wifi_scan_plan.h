#pragma once

#include <stdbool.h>
#include <stddef.h>

bool wifi_scan_should_disconnect(bool reconnecting);
void wifi_scan_hold_begin(void);
void wifi_scan_hold_end(void);
bool wifi_scan_reconnect_enabled(void);

typedef enum {
    WIFI_JOIN_PAUSE_RECONNECT = 0,
    WIFI_JOIN_DISCONNECT,
    WIFI_JOIN_SAVE,
    WIFI_JOIN_CONNECT,
    WIFI_JOIN_RESUME_RECONNECT,
} wifi_join_step_t;

size_t wifi_join_steps(wifi_join_step_t *out, size_t cap);
