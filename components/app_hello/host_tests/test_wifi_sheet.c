#include "wifi_sheet.h"

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

static void a_settled_pull_down_scans_once(void)
{
    wifi_sheet_t sheet;
    wifi_sheet_init(&sheet);
    expect(!wifi_sheet_take_scan(&sheet), "the recorder stays up until the sheet opens");
    wifi_sheet_note(&sheet, 1, true, false);
    expect(!wifi_sheet_take_scan(&sheet), "a pull still in motion does not scan");
    wifi_sheet_note(&sheet, 1, false, true);
    expect(wifi_sheet_take_scan(&sheet), "a settled pull-down scans once");
    expect(!wifi_sheet_take_scan(&sheet), "the open sheet does not scan again");
    wifi_sheet_note(&sheet, 1, false, true);
    expect(!wifi_sheet_take_scan(&sheet), "staying open is not another pull");
    wifi_sheet_note(&sheet, 0, false, true);
    expect(!wifi_sheet_take_scan(&sheet), "closing the sheet does not scan");
    expect(wifi_sheet_take_closed(&sheet), "closing the sheet leaves the network list");
    expect(!wifi_sheet_take_closed(&sheet), "staying closed is not another close");
    wifi_sheet_note(&sheet, 1, false, true);
    expect(wifi_sheet_take_scan(&sheet), "pulling down again scans again");
    expect(wifi_sheet_take_key_hide(&sheet), "pulling down again hides the keyboard");
}

static void the_sheet_names_the_connected_network(void)
{
    char text[80];
    wifi_sheet_status(text, sizeof(text), true, "secret", 2, "home");
    expect(strcmp(text, "Password secret") == 0, "password entry keeps the typed password");
    wifi_sheet_status(text, sizeof(text), false, "", 0, "");
    expect(strcmp(text, "No networks") == 0, "a finished empty scan names the empty list");
    wifi_sheet_status(text, sizeof(text), false, "", 2, "");
    expect(strcmp(text, "Tap a network") == 0, "a scan without a connection asks for a network");
    wifi_sheet_status(text, sizeof(text), false, "", 2, "home");
    expect(strcmp(text, "Connected home") == 0, "the sheet shows the network that is connected");
}

static void the_keyboard_waits_for_a_selected_network(void)
{
    wifi_sheet_t sheet;
    wifi_sheet_init(&sheet);
    expect(!wifi_sheet_keyboard_visible(false, false), "the home screen has no keyboard");
    expect(!wifi_sheet_keyboard_visible(false, true), "the network list has no keyboard");
    expect(!wifi_sheet_keyboard_visible(true, false), "a closed sheet hides the keyboard");
    expect(wifi_sheet_keyboard_visible(true, true), "a selected locked network shows the keyboard");
    wifi_sheet_note(&sheet, 1, false, true);
    expect(wifi_sheet_take_key_hide(&sheet), "opening the sheet hides keys that sit on the list");
    expect(!wifi_sheet_take_key_hide(&sheet), "an open sheet hides the keys once");
}

int main(void)
{
    a_settled_pull_down_scans_once();
    the_keyboard_waits_for_a_selected_network();
    the_sheet_names_the_connected_network();
    if (g_failed) {
        fprintf(stderr, "%d wifi sheet checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
