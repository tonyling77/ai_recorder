#include "wifi_picker.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void strongest_unique_networks_come_first(void)
{
    wifi_picker_t picker;
    wifi_network_t scan[] = {
        {.ssid = "weak", .rssi = -80, .open = false},
        {.ssid = "home", .rssi = -70, .open = false},
        {.ssid = "home", .rssi = -40, .open = false},
        {.ssid = "cafe", .rssi = -50, .open = true},
        {.ssid = "", .rssi = -20, .open = true},
    };
    wifi_picker_init(&picker);
    wifi_picker_set_scan(&picker, scan, sizeof(scan) / sizeof(scan[0]));
    expect(wifi_picker_count(&picker) == 3, "blank and duplicate ssids are dropped");
    expect(strcmp(picker.networks[0].ssid, "home") == 0 && picker.networks[0].rssi == -40,
           "the stronger duplicate is kept and sorted first");
    expect(strcmp(picker.networks[1].ssid, "cafe") == 0 && picker.networks[1].open,
           "an open network stays marked open");
}

static void an_open_network_can_join_immediately(void)
{
    wifi_picker_t picker;
    wifi_network_t scan[] = {
        {.ssid = "cafe", .rssi = -50, .open = true},
    };
    char ssid[33];
    char password[64];
    wifi_picker_init(&picker);
    wifi_picker_set_scan(&picker, scan, 1);
    expect(wifi_picker_tap(&picker, 0) == 1, "tapping an open network is ready to join");
    expect(wifi_picker_copy_join(&picker, ssid, sizeof(ssid), password, sizeof(password)) == 0,
           "the open network has a join request");
    expect(strcmp(ssid, "cafe") == 0 && password[0] == '\0', "an open network has no password");
}

static void a_locked_network_waits_for_a_password(void)
{
    wifi_picker_t picker;
    wifi_network_t scan[] = {
        {.ssid = "home", .rssi = -40, .open = false},
    };
    char ssid[33];
    char password[64];
    char key;
    wifi_picker_init(&picker);
    wifi_picker_set_scan(&picker, scan, 1);
    expect(wifi_picker_tap(&picker, 0) == 0, "a locked network asks for a password");
    expect(wifi_picker_copy_join(&picker, ssid, sizeof(ssid), password, sizeof(password)) != 0,
           "an empty password is not sent");
    expect(wifi_picker_count(&picker) == 39, "digits and the full letter keyboard are shown together");
    expect(wifi_picker_key(&picker, 0, &key) && key == '1', "the number row is on the keyboard");
    expect(wifi_picker_key(&picker, 10, &key) && key == 'q', "letters follow keyboard order");
    expect(wifi_picker_key(&picker, 20, &key) && key == 'a', "the home row is on the same keyboard");
    expect(strcmp(wifi_picker_shift_label(&picker), "ABC") == 0, "the shift key offers uppercase");
    wifi_picker_next_page(&picker);
    expect(wifi_picker_key(&picker, 10, &key) && key == 'Q', "shift shows the uppercase letter");
    expect(wifi_picker_key(&picker, 0, &key) && key == '1', "shift leaves the number row unchanged");
    expect(strcmp(wifi_picker_shift_label(&picker), "abc") == 0, "the shift key offers lowercase again");
    expect(wifi_picker_tap(&picker, 10) == 0, "an uppercase key appends");
    expect(strcmp(picker.password, "Q") == 0, "the password stores the uppercase letter");
    wifi_picker_backspace(&picker);
    wifi_picker_next_page(&picker);
    expect(wifi_picker_tap(&picker, 20) == 0, "a key appends instead of joining");
    expect(strcmp(picker.password, "a") == 0, "the password grows by one character");
    wifi_picker_backspace(&picker);
    expect(picker.password[0] == '\0', "backspace clears the last character");
    expect(wifi_picker_copy_join(&picker, ssid, sizeof(ssid), password, sizeof(password)) != 0,
           "clearing the password blocks the join");
    wifi_picker_tap(&picker, 20);
    expect(wifi_picker_copy_join(&picker, ssid, sizeof(ssid), password, sizeof(password)) == 0 &&
               strcmp(password, "a") == 0 && strcmp(ssid, "home") == 0,
           "a locked network joins with the entered password");
    wifi_picker_show_networks(&picker);
    expect(!picker.password_mode && wifi_picker_count(&picker) == 1, "closing the sheet returns to the network list");
    expect(strcmp(picker.networks[0].ssid, "home") == 0, "the scanned network stays available");
}

int main(void)
{
    strongest_unique_networks_come_first();
    an_open_network_can_join_immediately();
    a_locked_network_waits_for_a_password();
    if (g_failed) {
        fprintf(stderr, "%d wifi picker checks failed\n", g_failed);
    }
    return g_failed ? 1 : 0;
}
