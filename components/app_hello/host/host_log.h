/* The log of the desktop build.
 *
 * The application writes to the log of ESP-IDF on the board. The simulator
 * has no such log, so the calls go to the standard output here. That keeps
 * the application itself free of any build specific code, and it lets the
 * simulator show what the application is doing.
 *
 * The device keeps its own log, which is the one that matters for a fault
 * on the board. This file is only for the desktop build. */
#include <stdarg.h>
#include <stdio.h>

#define HOST_LOG_LEVEL_ERROR 1
#define HOST_LOG_LEVEL_WARN 2
#define HOST_LOG_LEVEL_INFO 3
#define HOST_LOG_LEVEL_DEBUG 4

/* The desktop build shows the warnings and above, like the board does. */
#ifndef HOST_LOG_LEVEL
#define HOST_LOG_LEVEL HOST_LOG_LEVEL_INFO
#endif

static void host_log(const char *level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s (%s): ", level, tag);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

#define HOST_LOG_E(tag, ...) host_log("E", tag, __VA_ARGS__)
#define HOST_LOG_W(tag, ...) host_log("W", tag, __VA_ARGS__)
#define HOST_LOG_I(tag, ...) host_log("I", tag, __VA_ARGS__)
#define HOST_LOG_D(tag, ...) host_log("D", tag, __VA_ARGS__)

#if HOST_LOG_LEVEL <= HOST_LOG_LEVEL_ERROR
#define ESP_LOGE(tag, format, ...) HOST_LOG_E(tag, format, ##__VA_ARGS__)
#else
#define ESP_LOGE(tag, format, ...)                                                      \
    do {                                                                                \
    } while (0)
#endif

#if HOST_LOG_LEVEL <= HOST_LOG_LEVEL_WARN
#define ESP_LOGW(tag, format, ...) HOST_LOG_W(tag, format, ##__VA_ARGS__)
#else
#define ESP_LOGW(tag, format, ...)                                                      \
    do {                                                                                \
    } while (0)
#endif

#if HOST_LOG_LEVEL <= HOST_LOG_LEVEL_INFO
#define ESP_LOGI(tag, format, ...) HOST_LOG_I(tag, format, ##__VA_ARGS__)
#else
#define ESP_LOGI(tag, format, ...)                                                      \
    do {                                                                                \
    } while (0)
#endif

#if HOST_LOG_LEVEL <= HOST_LOG_LEVEL_DEBUG
#define ESP_LOGD(tag, format, ...) HOST_LOG_D(tag, format, ##__VA_ARGS__)
#else
#define ESP_LOGD(tag, format, ...)                                                      \
    do {                                                                                \
    } while (0)
#endif
