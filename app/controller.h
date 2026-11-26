#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dsu/dsu_server.h>

typedef enum {
    APP_BUTTON_DPAD_LEFT = 1ULL << 0,
    APP_BUTTON_DPAD_DOWN = 1ULL << 1,
    APP_BUTTON_DPAD_RIGHT = 1ULL << 2,
    APP_BUTTON_DPAD_UP = 1ULL << 3,
    APP_BUTTON_OPTIONS = 1ULL << 4,
    APP_BUTTON_STICK_R = 1ULL << 5,
    APP_BUTTON_STICK_L = 1ULL << 6,
    APP_BUTTON_SHARE = 1ULL << 7,
    APP_BUTTON_Y = 1ULL << 8,
    APP_BUTTON_B = 1ULL << 9,
    APP_BUTTON_A = 1ULL << 10,
    APP_BUTTON_X = 1ULL << 11,
    APP_BUTTON_R1 = 1ULL << 12,
    APP_BUTTON_L1 = 1ULL << 13,
    APP_BUTTON_R2 = 1ULL << 14,
    APP_BUTTON_L2 = 1ULL << 15,
} AppButton;

typedef struct {
    bool connected;
    uint64_t buttons;
    bool home;
    bool touch;
    DsuTouchPoint touch_points[2];
    int32_t left_x;
    int32_t left_y;
    int32_t right_x;
    int32_t right_y;
    int32_t left_trigger;
    int32_t right_trigger;
    bool motion_valid;
    uint64_t motion_timestamp_us;
    float acceleration[3];
    float angular_velocity[3];
} AppControllerInput;

void app_controller_neutral(AppControllerInput *input);
void app_controller_to_dsu(const AppControllerInput *input,
    DsuControllerState *output);
