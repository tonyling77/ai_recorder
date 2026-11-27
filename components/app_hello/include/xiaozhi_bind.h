#pragma once

/* Start the check-in that binds the device to the cloud.
 *
 * A check-in that is already running is left alone: the cloud is not asked
 * for the device twice. */
void xiaozhi_bind_start(void);

/* End the check-in and wait for it to be gone.
 *
 * The check-in belongs to the Xiaozhi channel, so it is ended when the
 * user chooses the other channel. The wait makes the memory of the check-in
 * free before the other channel starts its work.
 *
 * The caller must not be the task of the screen: the wait can take a
 * while, and the screen must keep answering. */
void xiaozhi_bind_stop(void);

char *xiaozhi_bind_take_body(void);
int xiaozhi_bind_take_status(int *status);
