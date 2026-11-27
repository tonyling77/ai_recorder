#pragma once

/* The inbox address over the link to the host.
 *
 * A developer flashes the board and runs the inbox on their own machine.
 * The address of that machine is not known when the firmware is built, so
 * the host tells the board where the inbox is, over the same link that
 * carried the update.
 *
 * The service id is in the range that the protocol reserves for the
 * application: 0x1001 is the pointer profile and 0x7fff is recovery. */

#define INBOX_CONFIG_RPC_SERVICE_ID 0x1002
#define INBOX_CONFIG_RPC_METHOD_SET 1

/* Register the handler. Call once, after NVS is open. */
void inbox_config_rpc_start(void);
