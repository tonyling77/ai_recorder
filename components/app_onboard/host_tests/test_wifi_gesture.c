#include "wifi_gesture.h"

#include <stdio.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void five_presses_inside_three_seconds_clear_wifi(void)
{
    wifi_gesture_t gesture;
    wifi_gesture_reset(&gesture);
    expect(wifi_gesture_note_press(&gesture, 0) == WIFI_GESTURE_IDLE, "first press waits");
    expect(wifi_gesture_note_press(&gesture, 400) == WIFI_GESTURE_IDLE, "second press waits");
    expect(wifi_gesture_note_press(&gesture, 800) == WIFI_GESTURE_IDLE, "third press waits");
    expect(wifi_gesture_note_press(&gesture, 1200) == WIFI_GESTURE_IDLE, "fourth press waits");
    expect(wifi_gesture_note_press(&gesture, 1600) == WIFI_GESTURE_CLEAR_CREDENTIALS,
           "fifth press inside the window clears Wi-Fi credentials");
}

static void a_gap_longer_than_three_seconds_starts_over(void)
{
    wifi_gesture_t gesture;
    wifi_gesture_reset(&gesture);
    expect(wifi_gesture_note_press(&gesture, 0) == WIFI_GESTURE_IDLE, "old press");
    expect(wifi_gesture_note_press(&gesture, 1000) == WIFI_GESTURE_IDLE, "second old press");
    expect(wifi_gesture_note_press(&gesture, 2000) == WIFI_GESTURE_IDLE, "third old press");
    expect(wifi_gesture_note_press(&gesture, 3001) == WIFI_GESTURE_IDLE, "the window expired");
    expect(wifi_gesture_note_press(&gesture, 3400) == WIFI_GESTURE_IDLE, "still short of five");
    expect(wifi_gesture_note_press(&gesture, 3800) == WIFI_GESTURE_IDLE, "still short of five");
    expect(wifi_gesture_note_press(&gesture, 4200) == WIFI_GESTURE_IDLE, "still short of five");
    expect(wifi_gesture_note_press(&gesture, 4600) == WIFI_GESTURE_CLEAR_CREDENTIALS,
           "five presses in the new window clear Wi-Fi credentials");
}

static void the_sequence_arms_again_only_after_another_five(void)
{
    wifi_gesture_t gesture;
    wifi_gesture_reset(&gesture);
    for (uint32_t i = 0; i < 4; i++) {
        expect(wifi_gesture_note_press(&gesture, i * 100) == WIFI_GESTURE_IDLE, "arming press");
    }
    expect(wifi_gesture_note_press(&gesture, 400) == WIFI_GESTURE_CLEAR_CREDENTIALS, "triggered");
    expect(wifi_gesture_note_press(&gesture, 500) == WIFI_GESTURE_IDLE, "the next press starts over");
}

int main(void)
{
    five_presses_inside_three_seconds_clear_wifi();
    a_gap_longer_than_three_seconds_starts_over();
    the_sequence_arms_again_only_after_another_five();
    expect(wifi_gesture_note_press(NULL, 0) == WIFI_GESTURE_IDLE, "missing gesture is ignored");
    return g_failed ? 1 : 0;
}
