#include "recorder_async.h"

#include <stdio.h>

static int failed;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        failed++;
    }
}

int main(void)
{
    recorder_async_t job;
    recorder_async_init(&job);

    expect(recorder_async_request(&job) == 0, "idle job accepts upload");
    expect(recorder_async_request(&job) != 0, "running job rejects a duplicate upload");
    recorder_async_complete(&job, 3);

    int result = -1;
    expect(recorder_async_take_result(&job, &result) == 1 && result == 3,
           "UI can consume the background result");
    expect(recorder_async_take_result(&job, &result) == 0, "result is consumed once");
    expect(recorder_async_request(&job) == 0, "job accepts another upload after completion");
    return failed ? 1 : 0;
}
