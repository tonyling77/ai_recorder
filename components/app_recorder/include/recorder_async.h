#pragma once

#include <stdbool.h>

typedef struct {
    bool running;
    bool result_ready;
    int result;
} recorder_async_t;

void recorder_async_init(recorder_async_t *job);
int recorder_async_request(recorder_async_t *job);
void recorder_async_complete(recorder_async_t *job, int result);
int recorder_async_take_result(recorder_async_t *job, int *result);
