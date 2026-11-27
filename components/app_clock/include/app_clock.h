#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The wall clock of the board.
 *
 * The RainMaker cloud client used to carry the time sync. It is off, because
 * its endless TLS reconnect loop drained the internal DMA pool and broke the
 * microphone. This component replaces that one clock source with a plain
 * lwIP SNTP client, so the device keeps a wall clock without the RainMaker
 * MQTT client coming back.
 *
 * The component owns the SNTP client and the clock state. app_onboard starts
 * it once the station has an address, and app_recorder asks it whether the
 * clock is usable before it opens a TLS connection. */

/* Start the SNTP client and keep retrying until the clock is set.
 *
 * Call it once the station has an IP address: SNTP needs a route. The call
 * never blocks the caller and never blocks the Wi-Fi join or the UI. */
void app_clock_start(void);

/* True when the system clock holds a plausible wall-clock time.
 *
 * A clock that was never set reads the epoch, so the callers use this to
 * keep a TLS handshake or a recording timestamp from pretending to have a
 * date that is 56 years in the past. */
bool app_clock_valid(void);

/* Write a human readable local wall-clock time, for logs. */
void app_clock_log(const char *prefix);
