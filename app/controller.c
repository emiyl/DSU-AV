#include "controller.h"

#include <string.h>

static uint8_t axis_to_dsu(int32_t value)
{
    if (value < -32768)
        value = -32768;
    if (value > 32767)
        value = 32767;
    return (uint8_t)((value + 32768) * 255 / 65535);
}

static uint8_t trigger_to_dsu(int32_t value)
{
    if (value < 0)
        value = 0;
    if (value > 32767)
        value = 32767;
    return (uint8_t)(value * 255 / 32767);
}

void app_controller_neutral(AppControllerInput *input)
{
    memset(input, 0, sizeof(*input));
    input->connected = true;
}

void app_controller_to_dsu(const AppControllerInput *input,
    DsuControllerState *output)
{
    memset(output, 0, sizeof(*output));
    output->connected = input->connected;
    output->home_button = input->home;
    output->touch_button = input->touch;
    output->left_stick_x = axis_to_dsu(input->left_x);
    output->left_stick_y = axis_to_dsu(input->left_y);
    output->right_stick_x = axis_to_dsu(input->right_x);
    output->right_stick_y = axis_to_dsu(input->right_y);

    for (unsigned i = 0; i < 8; ++i) {
        bool first_pressed = (input->buttons & (1ULL << i)) != 0;
        bool second_pressed =
            (input->buttons & (1ULL << (8 + i))) != 0;
        uint8_t first = first_pressed ? 255 : 0;
        uint8_t second = second_pressed ? 255 : 0;
        if (first_pressed)
            output->button_masks[0] |= (uint8_t)(0x80U >> i);
        if (second_pressed)
            output->button_masks[1] |= (uint8_t)(0x80U >> i);
        if (i < 4)
            output->analog_buttons[i] = first;
        output->analog_buttons[4 + i] = second;
    }
    output->analog_buttons[10] = trigger_to_dsu(input->right_trigger);
    output->analog_buttons[11] = trigger_to_dsu(input->left_trigger);
    if (output->analog_buttons[10])
        output->button_masks[1] |= 0x02;
    if (output->analog_buttons[11])
        output->button_masks[1] |= 0x01;

    output->motion_valid = input->motion_valid;
    output->motion_timestamp_us = input->motion_timestamp_us;
    if (input->motion_valid) {
        memcpy(output->acceleration, input->acceleration,
            sizeof(output->acceleration));
        memcpy(output->angular_velocity, input->angular_velocity,
            sizeof(output->angular_velocity));
    }
}
