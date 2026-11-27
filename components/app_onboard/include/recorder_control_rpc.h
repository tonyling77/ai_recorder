#pragma once

#include "app_hello.h"

/* The recorder over the link to the host.
 *
 * The recording is started and stopped by the person in front of the
 * board, and the upload is asked for with the same finger. That makes the
 * whole path hard to check from a host, and hard to check at all when the
 * board is on a bench and nobody is standing in front of it.
 *
 * This service lets the host ask for the same things the buttons ask
 * for, over the link that already carries the update. It is the same
 * request the interface makes, so it goes through the same queue and the
 * same code.
 *
 * The service id is in the range that the protocol reserves for the
 * application: 0x1001 is the pointer profile, 0x1002 is the inbox
 * address and 0x7fff is recovery. */

#define RECORDER_CONTROL_RPC_SERVICE_ID 0x1003
#define RECORDER_CONTROL_RPC_METHOD_COMMAND 1

/* The commands the interface can ask for. They are the same numbers the
 * application uses, so a host and the interface mean the same thing. */
#define RECORDER_CONTROL_COMMAND_TOGGLE_RECORD 0
#define RECORDER_CONTROL_COMMAND_UPLOAD 1
/* Read the state without doing anything. A host that waits for the
 * recorder to settle needs to look without touching it. */
#define RECORDER_CONTROL_COMMAND_STATE 2
/* Switch the channel, the way the settings page does. The number is the
 * channel: 0 is the private inbox and 1 is the Xiaozhi cloud. */
#define RECORDER_CONTROL_COMMAND_CHANNEL 3

/* Register the handler. Call once, after the recorder is open. */
void recorder_control_rpc_start(void);

/* Tell the handler which recorder it drives. Call once, after the
 * application is open. */
void recorder_control_rpc_set_recorder(app_hello_handle_t recorder);
