#include "node_presence.h"

#include <stdio.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

static void the_first_follow_up_repeats_online_only(void)
{
    node_presence_action_t action = node_presence_action(1);
    expect(action.report_online, "the first follow-up repeats online");
    expect(!action.bump_config, "the first follow-up keeps the config checksum");
}

static void the_second_follow_up_changes_the_config(void)
{
    node_presence_action_t action = node_presence_action(2);
    expect(action.bump_config, "the second follow-up changes ncfg_ver");
    expect(!action.report_online, "the config ack carries the new version");
}

static void the_third_follow_up_repeats_online_again(void)
{
    node_presence_action_t action = node_presence_action(3);
    expect(action.report_online, "a later follow-up repeats online");
    expect(!action.bump_config, "the config is bumped once");
}

static void other_attempts_do_nothing(void)
{
    node_presence_action_t action = node_presence_action(0);
    expect(!action.report_online && !action.bump_config, "attempt 0 is idle");
    action = node_presence_action(4);
    expect(!action.report_online && !action.bump_config, "attempt 4 is idle");
}

int main(void)
{
    the_first_follow_up_repeats_online_only();
    the_second_follow_up_changes_the_config();
    the_third_follow_up_repeats_online_again();
    other_attempts_do_nothing();
    return g_failed ? 1 : 0;
}
