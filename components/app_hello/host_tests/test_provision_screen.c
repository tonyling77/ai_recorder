#include "provision_screen.h"

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

static void offer_shows_the_qr(void)
{
    provision_screen_t screen;
    provision_screen_init(&screen);
    expect(!screen.visible && !screen.deferred && screen.payload[0] == '\0', "a new screen is hidden");
    expect(provision_screen_offer(&screen, NULL) == 0, "a missing payload stays hidden");
    expect(provision_screen_offer(&screen, "") == 0, "an empty payload stays hidden");
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 1, "a QR payload is shown");
    expect(screen.visible && !screen.deferred, "the panel is visible");
    expect(strcmp(screen.payload, "NP:PROV_7e2d10|522655ab|b") == 0, "the payload is kept");
}

static void skip_hides_the_qr_until_a_new_payload(void)
{
    provision_screen_t screen;
    char too_long[PROVISION_SCREEN_PAYLOAD_MAX + 8];
    provision_screen_init(&screen);
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 1, "show the first QR");
    provision_screen_skip(&screen);
    expect(!screen.visible && screen.deferred, "skip hides the QR for later");
    expect(strcmp(screen.payload, "NP:PROV_7e2d10|522655ab|b") == 0, "skip keeps the payload");
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 0, "the same QR stays dismissed");
    expect(!screen.visible && screen.deferred, "a repeated QR does not cover the recorder");
    expect(provision_screen_offer(&screen, "NP:PROV_next|00112233|b") == 1, "a new QR is shown");
    expect(screen.visible && !screen.deferred, "a new payload clears the skip");
    memset(too_long, 'A', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    expect(provision_screen_offer(&screen, too_long) == 0, "an oversized payload is ignored");
    expect(strcmp(screen.payload, "NP:PROV_next|00112233|b") == 0, "the current payload stays");
}

static void complete_returns_to_the_recorder(void)
{
    provision_screen_t screen;
    provision_screen_init(&screen);
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 1, "show the QR");
    provision_screen_complete(&screen);
    expect(!screen.visible && !screen.deferred, "success leaves the main page");
    expect(screen.payload[0] == '\0', "a finished QR is not kept for setup");
    expect(provision_screen_reopen(&screen) == 0, "setup cannot reopen a finished QR");
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 1, "a later provisioning QR is shown");
    provision_screen_skip(&screen);
    provision_screen_complete(&screen);
    expect(!screen.visible && !screen.deferred && screen.payload[0] == '\0', "success clears a skipped QR");
}

static void reopen_shows_the_stored_qr(void)
{
    provision_screen_t screen;
    provision_screen_init(&screen);
    expect(provision_screen_reopen(&screen) == 0, "nothing to reopen yet");
    expect(provision_screen_offer(&screen, "NP:PROV_7e2d10|522655ab|b") == 1, "show the QR");
    provision_screen_skip(&screen);
    provision_screen_skip(&screen);
    expect(!screen.visible && screen.deferred, "skipping again stays deferred");
    expect(provision_screen_reopen(&screen) == 1, "setup shows the stored QR");
    expect(screen.visible && !screen.deferred, "the QR is visible again");
    expect(strcmp(screen.payload, "NP:PROV_7e2d10|522655ab|b") == 0, "reopen uses the same payload");
}

int main(void)
{
    offer_shows_the_qr();
    skip_hides_the_qr_until_a_new_payload();
    complete_returns_to_the_recorder();
    reopen_shows_the_stored_qr();
    if (g_failed) {
        fprintf(stderr, "%d provision screen checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
