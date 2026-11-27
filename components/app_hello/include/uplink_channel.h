#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    UPLINK_CHANNEL_PRIVATE = 0,
    UPLINK_CHANNEL_XIAOZHI = 1,
    UPLINK_CHANNEL_ESP_AGENT = 2,
} uplink_channel_t;

uplink_channel_t uplink_channel_default(void);
bool uplink_channel_runs_private(uplink_channel_t channel);
bool uplink_channel_runs_xiaozhi(uplink_channel_t channel);
/* The name of the channel, as it is shown on the screen. */
const char *uplink_channel_label(uplink_channel_t channel);

/* The channel the device was on.
 *
 * The device comes back on the channel it was on, so a board that was put
 * on the Xiaozhi channel does not quietly go back to the private one after
 * a restart. The channel is kept in NVS, beside the address of the inbox. */
void uplink_channel_load(uplink_channel_t *out);
int uplink_channel_store(uplink_channel_t channel);

typedef struct {
    bool show_code;
    char code[7];
    char hint[48];
} xiaozhi_bind_view_t;

typedef struct {
    const char *mac;
    const char *uuid;
    const char *chip;
    const char *app_version;
} xiaozhi_ota_identity_t;

int xiaozhi_ota_request(char *out, size_t cap, const xiaozhi_ota_identity_t *id);
void xiaozhi_bind_view_waiting(xiaozhi_bind_view_t *out);
int xiaozhi_bind_view_from_ota(const char *json, xiaozhi_bind_view_t *out);
int xiaozhi_bind_from_http(int status);
/* 1 when the check-in reply says the device is already bound, 0 otherwise. */
int xiaozhi_bind_already_bound(const char *json);
/* Takes the socket address, the socket token and, when version is not NULL, the
 * protocol version the server asked for. A reply without a version keeps the
 * original protocol version 1. */
int xiaozhi_bind_websocket(const char *json, char *url, size_t url_cap, char *token, size_t token_cap,
                           int *version);
