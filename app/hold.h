#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t started_us;
} AppHold;

void app_hold_reset(AppHold *hold);
bool app_hold_update(AppHold *hold, bool pressed, uint64_t now_us,
    uint64_t duration_us);
