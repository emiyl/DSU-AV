#include "logging.h"

#include <SDL.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static FILE *log_file;
static SDL_LogOutputFunction previous_log_function;
static void *previous_log_context;
static bool logging_initialized;

static void log_to_file(void *context, int category, SDL_LogPriority priority,
    const char *message)
{
    (void)context;
    if (log_file) {
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        char timestamp[32] = "unknown-time";
        if (local)
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local);
        fprintf(log_file, "[%s] [%d:%d] %s\n", timestamp, category,
            priority, message);
        fflush(log_file);
    }
    if (previous_log_function)
        previous_log_function(previous_log_context, category, priority,
            message);
}

bool app_logging_init(void)
{
#ifdef __SWITCH__
    const char *path = "sdmc:/switch/dsu-controller.log";
#else
    const char *path = "dsu-controller.log";
#endif
    log_file = fopen(path, "w");
    if (!log_file)
        return false;
    SDL_LogGetOutputFunction(&previous_log_function,
        &previous_log_context);
    SDL_LogSetOutputFunction(log_to_file, NULL);
    logging_initialized = true;
    app_logging_write("=== DSU controller started ===");
    return true;
}

void app_logging_close(void)
{
    if (!logging_initialized)
        return;
    app_logging_write("=== DSU controller stopped ===");
    SDL_LogSetOutputFunction(previous_log_function, previous_log_context);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    logging_initialized = false;
}

void app_logging_write(const char *format, ...)
{
    if (!log_file || !format)
        return;

    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    char timestamp[32] = "unknown-time";
    if (local)
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local);
    fprintf(log_file, "[%s] ", timestamp);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(log_file, format, arguments);
    va_end(arguments);
    fputc('\n', log_file);
    fflush(log_file);
}
