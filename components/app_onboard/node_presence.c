#include "node_presence.h"

node_presence_action_t node_presence_action(int attempt)
{
    node_presence_action_t action = { false, false };
    if (attempt == 1 || attempt == 3) {
        action.report_online = true;
    } else if (attempt == 2) {
        action.bump_config = true;
    }
    return action;
}
