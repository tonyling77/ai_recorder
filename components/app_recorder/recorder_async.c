#include "recorder_async.h"

void recorder_async_init(recorder_async_t *job)
{
    if (job) {
        *job = (recorder_async_t) {0};
    }
}

int recorder_async_request(recorder_async_t *job)
{
    if (!job || job->running || job->result_ready) {
        return -1;
    }
    job->running = true;
    return 0;
}

void recorder_async_complete(recorder_async_t *job, int result)
{
    if (!job || !job->running) {
        return;
    }
    job->running = false;
    job->result = result;
    job->result_ready = true;
}

int recorder_async_take_result(recorder_async_t *job, int *result)
{
    if (!job || !result || !job->result_ready) {
        return 0;
    }
    *result = job->result;
    job->result_ready = false;
    return 1;
}
