#include "wifi_scan_plan.h"

static volatile bool s_hold;

bool wifi_scan_should_disconnect(bool reconnecting)
{
    return reconnecting;
}

void wifi_scan_hold_begin(void)
{
    s_hold = true;
}

void wifi_scan_hold_end(void)
{
    s_hold = false;
}

bool wifi_scan_reconnect_enabled(void)
{
    return !s_hold;
}

size_t wifi_join_steps(wifi_join_step_t *out, size_t cap)
{
    static const wifi_join_step_t steps[] = {
        WIFI_JOIN_PAUSE_RECONNECT,
        WIFI_JOIN_DISCONNECT,
        WIFI_JOIN_SAVE,
        WIFI_JOIN_CONNECT,
        WIFI_JOIN_RESUME_RECONNECT,
    };
    size_t count = sizeof(steps) / sizeof(steps[0]);
    if (!out || cap < count) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        out[i] = steps[i];
    }
    return count;
}
