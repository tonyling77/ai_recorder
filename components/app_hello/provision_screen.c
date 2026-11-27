// SPDX-License-Identifier: Apache-2.0

#include "provision_screen.h"

#include <string.h>

void provision_screen_init(provision_screen_t *screen)
{
    if (!screen) {
        return;
    }
    screen->visible = false;
    screen->deferred = false;
    screen->payload[0] = '\0';
}

int provision_screen_offer(provision_screen_t *screen, const char *payload)
{
    size_t length;
    if (!screen || !payload || payload[0] == '\0') {
        return 0;
    }
    length = strlen(payload);
    if (length >= PROVISION_SCREEN_PAYLOAD_MAX) {
        return 0;
    }
    if (screen->deferred && strcmp(screen->payload, payload) == 0) {
        return 0;
    }
    memcpy(screen->payload, payload, length + 1);
    screen->visible = true;
    screen->deferred = false;
    return 1;
}

void provision_screen_skip(provision_screen_t *screen)
{
    if (!screen || screen->payload[0] == '\0') {
        return;
    }
    screen->visible = false;
    screen->deferred = true;
}

int provision_screen_reopen(provision_screen_t *screen)
{
    if (!screen || screen->payload[0] == '\0') {
        return 0;
    }
    screen->visible = true;
    screen->deferred = false;
    return 1;
}

void provision_screen_complete(provision_screen_t *screen)
{
    if (!screen) {
        return;
    }
    screen->visible = false;
    screen->deferred = false;
    screen->payload[0] = '\0';
}
