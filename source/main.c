#include <dsu/dsu_server.h>
#include "switch_av.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include <switch.h>

#define MOTION_HANDLES 4
#define UI_REFRESH_NS 250000000ULL
#define SEND_INTERVAL_NS 10000000ULL
#define EXIT_HOLD_US 3000000ULL

static uint8_t axis_to_dsu(int32_t axis)
{
    int64_t value = (int64_t)axis + 32768;
    if (value < 0)
        value = 0;
    if (value > 65535)
        value = 65535;
    return (uint8_t)((value * 255 + 32767) / 65535);
}

static uint64_t clock_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
        (uint64_t)now.tv_nsec / 1000ULL;
}

static void initialize_motion(HidSixAxisSensorHandle handles[MOTION_HANDLES])
{
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(
            &handles[0], 1, HidNpadIdType_Handheld,
            HidNpadStyleTag_NpadHandheld)))
        hidStartSixAxisSensor(handles[0]);
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(
            &handles[1], 1, HidNpadIdType_No1,
            HidNpadStyleTag_NpadFullKey)))
        hidStartSixAxisSensor(handles[1]);
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(
            &handles[2], 2, HidNpadIdType_No1,
            HidNpadStyleTag_NpadJoyDual))) {
        hidStartSixAxisSensor(handles[2]);
        hidStartSixAxisSensor(handles[3]);
    }
}

static bool read_motion(const PadState *pad,
    const HidSixAxisSensorHandle handles[MOTION_HANDLES],
    HidSixAxisSensorState *state)
{
    uint64_t style = padGetStyleSet(pad);
    HidSixAxisSensorHandle handle;
    if (style & HidNpadStyleTag_NpadHandheld)
        handle = handles[0];
    else if (style & HidNpadStyleTag_NpadFullKey)
        handle = handles[1];
    else if (style & HidNpadStyleTag_NpadJoyDual) {
        uint64_t attributes = padGetAttributes(pad);
        if (attributes & HidNpadAttribute_IsLeftConnected)
            handle = handles[2];
        else if (attributes & HidNpadAttribute_IsRightConnected)
            handle = handles[3];
        else
            return false;
    } else
        return false;

    return hidGetSixAxisSensorStates(handle, state, 1) > 0;
}

static void analog_button_values(uint8_t values[16], uint64_t buttons)
{
    static const uint64_t button_map[12] = {
        HidNpadButton_Left, HidNpadButton_Down, HidNpadButton_Right,
        HidNpadButton_Up, HidNpadButton_Y, HidNpadButton_B,
        HidNpadButton_A, HidNpadButton_X, HidNpadButton_R,
        HidNpadButton_L, HidNpadButton_ZR, HidNpadButton_ZL,
    };
    for (unsigned i = 0; i < 12; ++i)
        values[i] = (buttons & button_map[i]) != 0 ? 255 : 0;
}

static void update_controller_state(DsuControllerState *state, const PadState *pad,
    const HidSixAxisSensorHandle handles[MOTION_HANDLES], uint64_t *buttons)
{
    memset(state, 0, sizeof(*state));
    state->connected = padIsConnected(pad);
    *buttons = padGetButtons(pad);
    const uint64_t first_buttons[8] = {
        HidNpadButton_Left, HidNpadButton_Down, HidNpadButton_Right,
        HidNpadButton_Up, HidNpadButton_Plus, HidNpadButton_StickR,
        HidNpadButton_StickL, HidNpadButton_Minus,
    };
    const uint64_t second_buttons[8] = {
        HidNpadButton_Y, HidNpadButton_B, HidNpadButton_A,
        HidNpadButton_X, HidNpadButton_R, HidNpadButton_L,
        HidNpadButton_ZR, HidNpadButton_ZL,
    };
    for (unsigned i = 0; i < 8; ++i) {
        if (*buttons & first_buttons[i])
            state->button_masks[0] |= (uint8_t)(0x80U >> i);
        if (*buttons & second_buttons[i])
            state->button_masks[1] |= (uint8_t)(0x80U >> i);
    }
    state->left_stick_x = axis_to_dsu(padGetStickPos(pad, 0).x);
    state->left_stick_y = axis_to_dsu(padGetStickPos(pad, 0).y);
    state->right_stick_x = axis_to_dsu(padGetStickPos(pad, 1).x);
    state->right_stick_y = axis_to_dsu(padGetStickPos(pad, 1).y);
    analog_button_values(state->analog_buttons, *buttons);

    HidSixAxisSensorState motion = {0};
    if (read_motion(pad, handles, &motion)) {
        state->motion_valid = true;
        state->motion_timestamp_us = clock_us();
        state->acceleration[0] = motion.acceleration.x;
        state->acceleration[1] = motion.acceleration.y;
        state->acceleration[2] = motion.acceleration.z;
        state->angular_velocity[0] = motion.angular_velocity.x * (180.0f / 3.14159265358979323846f);
        state->angular_velocity[1] = motion.angular_velocity.y * (180.0f / 3.14159265358979323846f);
        state->angular_velocity[2] = motion.angular_velocity.z * (180.0f / 3.14159265358979323846f);
    }
}

static void print_status(const char *ip_address, const DsuServer *server,
    const DsuControllerState *controller, const SwitchAv *media,
    uint64_t buttons)
{
    printf("\x1b[1;1H");
    printf("DSU Controller Server\n");
    printf("Platform : %s\n", DSU_SWITCH_PLATFORM_NAME);
    printf("Listen   : %s:%u (UDP)\n", ip_address, (unsigned)DSU_PORT);
    printf("Client   : %s\n",
        server->subscriptions > 0 ? server->last_client : "waiting for subscription");
    printf("Inputs   : %s\n", controller->connected ? "controller connected" :
        "controller disconnected");
    printf("DSU-AV   : %s%s\n", dsu_server_av_is_active(server) ?
        "session active" : "ready",
        switch_av_is_video_visible(media) ? " (video)" : "");
    printf("Sent     : %" PRIu32 " packets\n", server->packets_sent);
    printf("Buttons  : 0x%016" PRIx64 "\n", buttons);
    printf("L stick  : %3u, %3u\n", controller->left_stick_x,
        controller->left_stick_y);
    printf("R stick  : %3u, %3u\n", controller->right_stick_x,
        controller->right_stick_y);
    printf("\nPress A to enable/mute the microphone when requested.\n");
    printf("Hold PLUS + MINUS for 3 seconds to exit.\n");
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    HidSixAxisSensorHandle motion_handles[MOTION_HANDLES] = {0};
    initialize_motion(motion_handles);

    char ip_address[INET_ADDRSTRLEN] = "No network";
    Result nifm_result = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(nifm_result)) {
        uint32_t current_ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&current_ip))) {
            struct in_addr address = {.s_addr = current_ip};
            inet_ntop(AF_INET, &address, ip_address, sizeof(ip_address));
        }
    }

    DsuServer server = {0};
    Result socket_result = socketInitializeDefault();
    uint32_t server_id = (uint32_t)svcGetSystemTick();
    DsuServerConfig server_config = {
        .server_id = server_id,
        .controller_mac = {
            0x02, 0x00, 0x44, (uint8_t)(server_id >> 16),
            (uint8_t)(server_id >> 8), (uint8_t)server_id,
        },
    };
    bool server_ready = R_SUCCEEDED(socket_result) &&
        dsu_server_init(&server, &server_config);
    SwitchAv *media = server_ready ? switch_av_create(&server) : NULL;
    if (media) {
        dsu_server_configure_av(&server, switch_av_capabilities(media),
            switch_av_callbacks(media), media);
    }
    uint64_t frame = 0;
    uint64_t exit_hold_started_us = 0;
    uint64_t controller_buttons = 0;
    DsuControllerState controller = {0};

    if (!server_ready) {
        printf("DSU server could not start.\n");
        printf("NIFM result: 0x%08x\n", nifm_result);
        printf("Socket result: 0x%08x\n", socket_result);
        printf("Check the network connection.\n");
        printf("Hold PLUS + MINUS for 3 seconds to exit.\n");
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        uint64_t buttons = padGetButtons(&pad);
        uint64_t buttons_down = padGetButtonsDown(&pad);
        uint64_t now_us = clock_us();
        bool exit_buttons_held =
            (buttons & (HidNpadButton_Plus | HidNpadButton_Minus)) ==
            (HidNpadButton_Plus | HidNpadButton_Minus);
        if (!exit_buttons_held)
            exit_hold_started_us = 0;
        else if (exit_hold_started_us == 0)
            exit_hold_started_us = now_us;
        else if (now_us - exit_hold_started_us >= EXIT_HOLD_US)
            break;

        switch_av_handle_input(media, buttons_down);
        update_controller_state(&controller, &pad, motion_handles,
            &controller_buttons);
        if (server_ready) {
            dsu_server_poll(&server, &controller);
            switch_av_poll(media);
            dsu_server_broadcast(&server, &controller);
            dsu_server_expire_clients(&server);
        }

        if ((frame++ % 25) == 0 && server_ready &&
            !switch_av_is_video_visible(media))
            print_status(ip_address, &server, &controller, media,
                controller_buttons);

        if (!switch_av_is_video_visible(media))
            consoleUpdate(NULL);
        svcSleepThread(SEND_INTERVAL_NS);
    }

    dsu_server_close(&server);
    switch_av_destroy(media);
    if (R_SUCCEEDED(socket_result))
        socketExit();
    if (R_SUCCEEDED(nifm_result))
        nifmExit();
    for (unsigned i = 0; i < MOTION_HANDLES; ++i) {
        if (motion_handles[i].type_value != 0)
            hidStopSixAxisSensor(motion_handles[i]);
    }
    consoleExit(NULL);
    return 0;
}
