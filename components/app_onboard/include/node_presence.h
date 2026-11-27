#pragma once

#include <stdbool.h>

typedef struct {
    bool report_online;
    bool bump_config;
} node_presence_action_t;

/* The phone marks a node online only from a shadow that contains online:true,
 * and refetches the node config only after ncfg_ver changes past a baseline.
 * The SDK's first publish happens before that subscription exists. */
node_presence_action_t node_presence_action(int attempt);
