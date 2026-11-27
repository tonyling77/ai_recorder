#include "wifi_key_place.h"

#include <stdio.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void the_keyboard_stays_off_the_network_list(void)
{
    wifi_key_place_t parked = wifi_key_place(0, false);
    wifi_key_place_t first = wifi_key_place(0, true);
    wifi_key_place_t q = wifi_key_place(10, true);
    wifi_key_place_t a = wifi_key_place(20, true);
    wifi_key_place_t z = wifi_key_place(29, true);
    wifi_key_place_t last = wifi_key_place(38, true);
    expect(parked.y - 480 >= 480, "a closed drawer does not leave a key on the home screen");
    expect(first.x == 12 && first.y == 104, "the number row starts at the top of the keyboard");
    expect(q.y == 158, "the q row is below the numbers");
    expect(a.x == 35 && a.y == 212, "the home row is centered");
    expect(z.y == 266 && last.x == 426 && last.y == 266, "the bottom row stays above Join");
}

int main(void)
{
    the_keyboard_stays_off_the_network_list();
    if (g_failed) {
        fprintf(stderr, "%d wifi key place checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
