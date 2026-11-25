#include "hold.h"

void app_hold_reset(AppHold *hold)
{
    hold->started_us = 0;
}

bool app_hold_update(AppHold *hold, bool pressed, uint64_t now_us,
    uint64_t duration_us)
{
    if (!pressed) {
        app_hold_reset(hold);
        return false;
    }
    if (hold->started_us == 0) {
        hold->started_us = now_us;
        return false;
    }
    return now_us - hold->started_us >= duration_us;
}
