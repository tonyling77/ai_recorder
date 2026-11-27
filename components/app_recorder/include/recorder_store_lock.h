#pragma once

#include <stdint.h>

/* The /nand LittleFS volume is one shared filesystem with no internal
 * locking, so every access to the recordings tree has to be serialised
 * through this single mutex.
 *
 * Contract:
 *  - The lock is recursive. A store call that internally calls another
 *    locked store function may take it again from the same task.
 *  - The lock must never be held across a blocking network operation.
 *    The upload path releases it before every HTTP request.
 *  - The lock must never be held while another mutex that guards the same
 *    tree is taken. There is exactly one lock for the recordings tree.
 *
 * recorder_store_lock_init() is idempotent and is called from
 * recorder_volume_prepare(), before any store use.
 */

/* How long a store call waits for the lock. The tree is only touched for a
 * short time, so a waiter that is still blocked after this has met a stuck
 * writer and reports an error instead of freezing the caller. */
#define RECORDER_STORE_LOCK_TIMEOUT_MS 2000

/* Create the mutex. Safe to call more than once. */
void recorder_store_lock_init(void);

/* Take the lock. timeout_ms 0 does not wait. Returns 0 when the lock was
 * taken and -1 when the wait ran out. */
int recorder_store_lock_acquire(uint32_t timeout_ms);

/* Release the lock once. Every successful acquire needs one release. */
void recorder_store_lock_release(void);
