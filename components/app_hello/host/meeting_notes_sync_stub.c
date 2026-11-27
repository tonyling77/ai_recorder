/* The notes sync is a device feature: it talks to the inbox over HTTP.
 *
 * The simulator has no inbox, so the controller gets a stub that keeps the
 * desktop build working. The minutes on the simulator come from the notes
 * document the simulator reads for itself, not from a server. */
#include "meeting_notes_sync.h"

#include <stddef.h>

void meeting_notes_sync_start(void)
{
}

char *meeting_notes_sync_take(void)
{
    return NULL;
}
