#pragma once

#include <stddef.h>
#include <stdio.h>
#include <time.h>

static inline int recorder_stamp_format(int year, int month, int day, int hour, int minute, char *out, size_t cap)
{
    int written;

    if (!out || cap < 14 || year < 2026 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    written = snprintf(out, cap, "%04d%02d%02d-%02d%02d", year, month, day, hour, minute);
    if (written != 13) {
        return -1;
    }
    return 0;
}

static inline int recorder_stamp_now(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm local;

    if (now < 0 || localtime_r(&now, &local) == NULL) {
        return -1;
    }
    return recorder_stamp_format(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, out,
                                 cap);
}
