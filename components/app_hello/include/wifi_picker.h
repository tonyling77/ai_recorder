#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PICKER_MAX 8
#define WIFI_PICKER_PASSWORD_MAX 63

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_network_t;

typedef struct {
    wifi_network_t networks[WIFI_PICKER_MAX];
    size_t count;
    int selected;
    bool password_mode;
    char password[WIFI_PICKER_PASSWORD_MAX + 1];
    unsigned key_page;
} wifi_picker_t;

void wifi_picker_init(wifi_picker_t *picker);
void wifi_picker_set_scan(wifi_picker_t *picker, const wifi_network_t *scan, size_t count);
void wifi_picker_show_networks(wifi_picker_t *picker);
size_t wifi_picker_count(const wifi_picker_t *picker);
int wifi_picker_tap(wifi_picker_t *picker, size_t index);
bool wifi_picker_key(const wifi_picker_t *picker, size_t index, char *out);
const char *wifi_picker_shift_label(const wifi_picker_t *picker);
void wifi_picker_backspace(wifi_picker_t *picker);
void wifi_picker_next_page(wifi_picker_t *picker);
int wifi_picker_copy_join(const wifi_picker_t *picker, char *ssid, size_t ssid_cap, char *password,
                          size_t password_cap);
