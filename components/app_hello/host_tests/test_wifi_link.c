#include "wifi_link.h"

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

static void the_corner_shows_whether_wifi_is_connected(void)
{
    expect(strcmp(wifi_link_text(true), "Wi-Fi") == 0, "a connected station shows Wi-Fi");
    expect(strcmp(wifi_link_text(false), "Offline") == 0, "no station shows Offline");
}

int main(void)
{
    the_corner_shows_whether_wifi_is_connected();
    if (g_failed) {
        fprintf(stderr, "%d wifi link checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
