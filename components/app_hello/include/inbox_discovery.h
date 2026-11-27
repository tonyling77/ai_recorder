#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Finding the inbox on the network.
 *
 * A developer runs the inbox on their own machine and the inbox announces
 * itself as _esp-inbox._tcp. The board looks for that service type, so the
 * address of the machine does not have to be known when the firmware is
 * built and may change under the board.
 *
 * A found inbox is never used without the user saying so. The candidates
 * are kept here, and the application confirms one before it becomes the
 * address the device talks to. */

/* How many candidates are kept. */
#define INBOX_DISCOVERY_MAX 4

/* Start looking. Call once, after the network is up. */
void inbox_discovery_start(void);

/* How many candidates were found. */
size_t inbox_discovery_count(void);

/* The candidate at the index: the origin to use and the name to show. */
int inbox_discovery_get(size_t index, char *origin, size_t origin_cap, char *name, size_t name_cap);

/* True when a candidate was found that the user has not confirmed yet. */
bool inbox_discovery_pending(void);

/* Use the candidate at the index and remember it.
 *
 * The address is stored, so it survives a restart. Returns 0 when it was
 * stored. */
int inbox_discovery_confirm(size_t index);
