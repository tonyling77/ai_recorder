#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool open;
    bool scan_pending;
    bool key_hide_pending;
    bool closed_pending;
} wifi_sheet_t;

void wifi_sheet_init(wifi_sheet_t *sheet);
void wifi_sheet_note(wifi_sheet_t *sheet, uint16_t value, bool dragging, bool finished);
bool wifi_sheet_take_scan(wifi_sheet_t *sheet);
bool wifi_sheet_take_key_hide(wifi_sheet_t *sheet);
bool wifi_sheet_take_closed(wifi_sheet_t *sheet);
bool wifi_sheet_keyboard_visible(bool password_mode, bool drawer_open);
void wifi_sheet_status(char *out, size_t cap, bool password_mode, const char *password, size_t networks,
                       const char *connected_ssid);
