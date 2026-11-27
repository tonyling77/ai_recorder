// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

/* RainMaker QR payloads are short. Longer text is rejected. */
#define PROVISION_SCREEN_PAYLOAD_MAX 160

typedef struct {
    bool visible;
    bool deferred;
    char payload[PROVISION_SCREEN_PAYLOAD_MAX];
} provision_screen_t;

void provision_screen_init(provision_screen_t *screen);

/* Show the QR for a new payload. The same payload stays hidden after Skip. */
int provision_screen_offer(provision_screen_t *screen, const char *payload);

/* Hide the QR and keep the payload so setup can resume later. */
void provision_screen_skip(provision_screen_t *screen);

/* Show the stored QR again. Returns 0 when nothing was stored. */
int provision_screen_reopen(provision_screen_t *screen);

/* Wi-Fi joined. Hide the QR and do not leave a Setup shortcut. */
void provision_screen_complete(provision_screen_t *screen);
