#pragma once

/* Download delivered meeting cards. The caller owns the returned text. */
char *meeting_notes_sync_take(void);
void meeting_notes_sync_start(void);

/* End the pull of the minutes and wait for it to be gone.
 *
 * The pull belongs to the private channel, so it is ended when the user
 * chooses the other channel. The wait makes the memory of the pull free
 * before the other channel starts its work.
 *
 * The caller must not be the task of the screen: the wait can take a
 * while, and the screen must keep answering. */
void meeting_notes_sync_stop(void);
