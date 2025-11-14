#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void nvfc_vlog(FILE *stream, const char *level, const char *fmt, va_list args) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stream, "[%s] [%s] ", ts, level);
    vfprintf(stream, fmt, args);
    fputc('\n', stream);
    fflush(stream);
}

void nvfc_log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    nvfc_vlog(stdout, "INFO", fmt, args);
    va_end(args);
}

void nvfc_log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    nvfc_vlog(stderr, "WARN", fmt, args);
    va_end(args);
}

void nvfc_log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    nvfc_vlog(stderr, "ERROR", fmt, args);
    va_end(args);
}
