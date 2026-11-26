#pragma once

#include <stdbool.h>

bool app_logging_init(void);
void app_logging_close(void);
void app_logging_write(const char *format, ...);
