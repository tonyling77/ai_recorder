#include "wifi_picker.h"

#include <stdio.h>
#include <string.h>

/* One screen: digits, then q w e r t y, a s d f, z x c v, then - . _ */
static const char KEYS[] = "1234567890qwertyuiopasdfghjklzxcvbnm-._";

void wifi_picker_init(wifi_picker_t *picker)
{
    if (!picker) {
        return;
    }
    memset(picker, 0, sizeof(*picker));
    picker->selected = -1;
}

static int find_ssid(wifi_picker_t *picker, const char *ssid)
{
    for (size_t i = 0; i < picker->count; i++) {
        if (strcmp(picker->networks[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void wifi_picker_show_networks(wifi_picker_t *picker)
{
    if (!picker) {
        return;
    }
    picker->password_mode = false;
    picker->password[0] = '\0';
    picker->key_page = 0;
}

void wifi_picker_set_scan(wifi_picker_t *picker, const wifi_network_t *scan, size_t count)
{
    if (!picker) {
        return;
    }
    picker->count = 0;
    picker->selected = -1;
    picker->password_mode = false;
    picker->password[0] = '\0';
    picker->key_page = 0;
    if (!scan) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (scan[i].ssid[0] == '\0') {
            continue;
        }
        int existing = find_ssid(picker, scan[i].ssid);
        if (existing >= 0) {
            if (scan[i].rssi > picker->networks[existing].rssi) {
                picker->networks[existing] = scan[i];
            }
            continue;
        }
        if (picker->count >= WIFI_PICKER_MAX) {
            size_t weakest = 0;
            for (size_t j = 1; j < picker->count; j++) {
                if (picker->networks[j].rssi < picker->networks[weakest].rssi) {
                    weakest = j;
                }
            }
            if (scan[i].rssi <= picker->networks[weakest].rssi) {
                continue;
            }
            picker->networks[weakest] = scan[i];
        } else {
            picker->networks[picker->count++] = scan[i];
        }
    }
    for (size_t i = 1; i < picker->count; i++) {
        wifi_network_t item = picker->networks[i];
        size_t j = i;
        while (j > 0 && picker->networks[j - 1].rssi < item.rssi) {
            picker->networks[j] = picker->networks[j - 1];
            j--;
        }
        picker->networks[j] = item;
    }
}

size_t wifi_picker_count(const wifi_picker_t *picker)
{
    if (!picker) {
        return 0;
    }
    if (picker->password_mode) {
        return strlen(KEYS);
    }
    return picker->count;
}

static char key_at(const wifi_picker_t *picker, size_t index)
{
    char key = KEYS[index];
    if (picker->key_page != 0 && key >= 'a' && key <= 'z') {
        key = (char)(key - 'a' + 'A');
    }
    return key;
}

int wifi_picker_tap(wifi_picker_t *picker, size_t index)
{
    if (!picker || index >= wifi_picker_count(picker)) {
        return -1;
    }
    if (picker->password_mode) {
        if (strlen(picker->password) >= WIFI_PICKER_PASSWORD_MAX) {
            return 0;
        }
        size_t used = strlen(picker->password);
        picker->password[used] = key_at(picker, index);
        picker->password[used + 1] = '\0';
        return 0;
    }
    picker->selected = (int)index;
    picker->password[0] = '\0';
    if (picker->networks[index].open) {
        return 1;
    }
    picker->password_mode = true;
    picker->key_page = 0;
    return 0;
}

bool wifi_picker_key(const wifi_picker_t *picker, size_t index, char *out)
{
    if (!picker || !picker->password_mode || !out || index >= wifi_picker_count(picker)) {
        return false;
    }
    *out = key_at(picker, index);
    return true;
}

const char *wifi_picker_shift_label(const wifi_picker_t *picker)
{
    if (!picker || !picker->password_mode) {
        return "Scan";
    }
    return picker->key_page == 0 ? "ABC" : "abc";
}

void wifi_picker_next_page(wifi_picker_t *picker)
{
    if (!picker || !picker->password_mode) {
        return;
    }
    picker->key_page ^= 1u;
}

void wifi_picker_backspace(wifi_picker_t *picker)
{
    size_t used;
    if (!picker) {
        return;
    }
    used = strlen(picker->password);
    if (used > 0) {
        picker->password[used - 1] = '\0';
    }
}

int wifi_picker_copy_join(const wifi_picker_t *picker, char *ssid, size_t ssid_cap, char *password,
                          size_t password_cap)
{
    const wifi_network_t *network;
    if (!picker || picker->selected < 0 || (size_t)picker->selected >= picker->count || !ssid || !password ||
        ssid_cap < 33 || password_cap < sizeof(picker->password)) {
        return -1;
    }
    network = &picker->networks[picker->selected];
    if (!network->open && picker->password[0] == '\0') {
        return -1;
    }
    snprintf(ssid, ssid_cap, "%s", network->ssid);
    snprintf(password, password_cap, "%s", network->open ? "" : picker->password);
    return 0;
}
