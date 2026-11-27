#include "wifi_sheet.h"

#include <stdio.h>

void wifi_sheet_init(wifi_sheet_t *sheet)
{
    if (sheet) {
        sheet->open = false;
        sheet->scan_pending = false;
        sheet->key_hide_pending = false;
        sheet->closed_pending = false;
    }
}

void wifi_sheet_note(wifi_sheet_t *sheet, uint16_t value, bool dragging, bool finished)
{
    bool open;
    if (!sheet) {
        return;
    }
    open = value != 0 && !dragging;
    if (finished && open && !sheet->open) {
        sheet->scan_pending = true;
        sheet->key_hide_pending = true;
    }
    if (!dragging && sheet->open && value == 0) {
        sheet->closed_pending = true;
    }
    if (!dragging) {
        sheet->open = value != 0;
    }
}

bool wifi_sheet_take_scan(wifi_sheet_t *sheet)
{
    bool pending;
    if (!sheet) {
        return false;
    }
    pending = sheet->scan_pending;
    sheet->scan_pending = false;
    return pending;
}

bool wifi_sheet_take_key_hide(wifi_sheet_t *sheet)
{
    bool pending;
    if (!sheet) {
        return false;
    }
    pending = sheet->key_hide_pending;
    sheet->key_hide_pending = false;
    return pending;
}

bool wifi_sheet_take_closed(wifi_sheet_t *sheet)
{
    bool pending;
    if (!sheet) {
        return false;
    }
    pending = sheet->closed_pending;
    sheet->closed_pending = false;
    return pending;
}

bool wifi_sheet_keyboard_visible(bool password_mode, bool drawer_open)
{
    return password_mode && drawer_open;
}

void wifi_sheet_status(char *out, size_t cap, bool password_mode, const char *password, size_t networks,
                       const char *connected_ssid)
{
    if (!out || cap == 0) {
        return;
    }
    if (password_mode) {
        snprintf(out, cap, "Password %s", password ? password : "");
    } else if (connected_ssid && connected_ssid[0] != '\0') {
        snprintf(out, cap, "Connected %s", connected_ssid);
    } else if (networks == 0) {
        snprintf(out, cap, "No networks");
    } else {
        snprintf(out, cap, "Tap a network");
    }
}
