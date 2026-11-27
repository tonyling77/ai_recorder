#include "ai_key_wifi.h"

#include <stdio.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static ai_key_wifi_action_t tap(ai_key_wifi_t *key, uint32_t down_ms)
{
    expect(ai_key_wifi_sample(key, true, down_ms) == AI_KEY_WIFI_NONE, "holding the key does not clear");
    return ai_key_wifi_sample(key, false, down_ms + 80);
}

static void five_short_presses_clear_wifi(void)
{
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    expect(tap(&key, 0) == AI_KEY_WIFI_NONE, "first short press waits");
    expect(tap(&key, 400) == AI_KEY_WIFI_NONE, "second short press waits");
    expect(tap(&key, 800) == AI_KEY_WIFI_NONE, "third short press waits");
    expect(tap(&key, 1200) == AI_KEY_WIFI_NONE, "fourth short press waits");
    expect(tap(&key, 1600) == AI_KEY_WIFI_CLEAR, "fifth short press clears Wi-Fi");
}

static void a_long_hold_does_not_count(void)
{
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    expect(ai_key_wifi_sample(&key, true, 0) == AI_KEY_WIFI_NONE, "press down waits");
    expect(ai_key_wifi_sample(&key, true, 500) == AI_KEY_WIFI_NONE, "still holding");
    expect(ai_key_wifi_sample(&key, false, 1000) == AI_KEY_WIFI_NONE, "a long hold is not a short press");
    expect(tap(&key, 1200) == AI_KEY_WIFI_NONE, "the next short press starts at one");
}

static void one_short_press_after_a_pause_toggles_record(void)
{
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    expect(tap(&key, 0) == AI_KEY_WIFI_NONE, "the press itself waits");
    expect(ai_key_wifi_sample(&key, false, 80 + AI_KEY_RECORD_GAP_MS - 1) == AI_KEY_WIFI_NONE,
           "a pause shorter than the gap still waits");
    expect(ai_key_wifi_sample(&key, false, 80 + AI_KEY_RECORD_GAP_MS) == AI_KEY_WIFI_RECORD,
           "one short press toggles recording");
    expect(ai_key_wifi_sample(&key, false, 80 + AI_KEY_RECORD_GAP_MS + 10) == AI_KEY_WIFI_NONE,
           "the same press does not toggle twice");
}

static void two_presses_do_not_toggle_record(void)
{
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    expect(tap(&key, 0) == AI_KEY_WIFI_NONE, "first of two");
    expect(tap(&key, 400) == AI_KEY_WIFI_NONE, "second arrives before the record gap");
    expect(ai_key_wifi_sample(&key, false, 480 + AI_KEY_RECORD_GAP_MS) == AI_KEY_WIFI_NONE,
           "an unfinished reset sequence does not record");
}

static void a_gap_restarts_the_count(void)
{
    ai_key_wifi_t key;
    ai_key_wifi_reset(&key);
    expect(tap(&key, 0) == AI_KEY_WIFI_NONE, "old press");
    expect(tap(&key, 3200) == AI_KEY_WIFI_NONE, "the window expired");
    expect(tap(&key, 3600) == AI_KEY_WIFI_NONE, "second of the new window");
    expect(tap(&key, 4000) == AI_KEY_WIFI_NONE, "third of the new window");
    expect(tap(&key, 4400) == AI_KEY_WIFI_NONE, "fourth of the new window");
    expect(tap(&key, 4800) == AI_KEY_WIFI_CLEAR, "five short presses in the new window clear Wi-Fi");
}

int main(void)
{
    five_short_presses_clear_wifi();
    a_long_hold_does_not_count();
    one_short_press_after_a_pause_toggles_record();
    two_presses_do_not_toggle_record();
    a_gap_restarts_the_count();
    expect(ai_key_wifi_sample(NULL, true, 0) == AI_KEY_WIFI_NONE, "missing key is ignored");
    return g_failed ? 1 : 0;
}
