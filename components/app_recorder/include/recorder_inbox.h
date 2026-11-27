#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The address of the inbox that the device talks to.
 *
 * The address is not a build input. A board that was flashed once works
 * with whatever machine the developer's inbox runs on, and the address of a
 * laptop changes when the lease runs out. The learned address is kept in
 * NVS, so it survives a restart and a firmware update.
 *
 * The learned origin is a name whenever the discovery can give one, for
 * example http://laptop.local:8765. A name is resolved on every request, so
 * the address of the machine may change under it.
 *
 * When nothing has been learned, the build-time origin is used. */

/* The longest origin that fits, including the trailing zero. */
#define RECORDER_INBOX_ORIGIN_MAX 128
#define RECORDER_INBOX_NAME_MAX 64

/* Read the learned origin from NVS. Call once, after NVS is open. */
void recorder_inbox_init(void);

/* The learned origin and the name of the machine that serves it.
 *
 * The origin is "" when nothing has been learned. Returns 0 on success. */
int recorder_inbox_get(char *origin, size_t origin_cap, char *name, size_t name_cap);

/* Learn an origin and remember it.
 *
 * The origin has to be a base URL, http:// or https://, with no path and no
 * query. Returns 0 when it was stored, -1 when it was refused. */
int recorder_inbox_set(const char *origin, const char *name);

/* Forget the learned origin. The build-time origin is used again. */
void recorder_inbox_clear(void);

/* True when an origin has been learned. */
bool recorder_inbox_known(void);
