#include "wifi_scan_plan.h"

#include <stdio.h>

static int g_failed;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        g_failed++;
    }
}

static void a_reconnect_loop_must_quiet_before_scan(void)
{
    expect(wifi_scan_should_disconnect(true), "a reconnecting station disconnects before the scan");
    expect(!wifi_scan_should_disconnect(false), "an idle station can scan without disconnecting first");
}

static void a_scan_hold_stops_the_reconnect_loop(void)
{
    expect(wifi_scan_reconnect_enabled(), "reconnect stays on while the drawer is closed");
    wifi_scan_hold_begin();
    expect(!wifi_scan_reconnect_enabled(), "reconnect pauses while a scan is running");
    wifi_scan_hold_end();
    expect(wifi_scan_reconnect_enabled(), "reconnect resumes after the scan");
}

static void a_new_password_is_saved_after_the_station_disconnects(void)
{
    wifi_join_step_t steps[5];
    size_t count = wifi_join_steps(steps, 5);
    expect(count == 5, "joining has one pause, disconnect, save, connect, and resume");
    expect(steps[0] == WIFI_JOIN_PAUSE_RECONNECT, "reconnect pauses before the password is saved");
    expect(steps[1] == WIFI_JOIN_DISCONNECT, "the station leaves connecting before the password is saved");
    expect(steps[2] == WIFI_JOIN_SAVE, "the password is saved after the station disconnects");
    expect(steps[3] == WIFI_JOIN_CONNECT, "the station connects with the saved password");
    expect(steps[4] == WIFI_JOIN_RESUME_RECONNECT, "reconnect resumes after the new network is started");
}

int main(void)
{
    a_reconnect_loop_must_quiet_before_scan();
    a_scan_hold_stops_the_reconnect_loop();
    a_new_password_is_saved_after_the_station_disconnects();
    if (g_failed) {
        fprintf(stderr, "%d failed\n", g_failed);
        return 1;
    }
    return 0;
}
