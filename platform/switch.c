#include <dsu/dsu_server.h>
#include "switch_av.h"
#include "../app/platform.h"
#include "../app/sdl_display.h"
#include "../app/controller.h"
#include "../app/hold.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include <switch.h>

#define MOTION_HANDLES 4
#define UI_REFRESH_NS 250000000ULL
#define SEND_INTERVAL_NS 10000000ULL
#define EXIT_HOLD_US 3000000ULL

struct AppPlatform {
    SdlDisplay *display;
    PadState pad;
    HidSixAxisSensorHandle motion_handles[MOTION_HANDLES];
    char ip_address[INET_ADDRSTRLEN];
    DsuServerConfig config;
    SwitchAv *media;
    bool socket_initialized;
    bool nifm_initialized;
    uint64_t frame;
    uint64_t controller_buttons;
    AppHold exit_hold;
};

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

static void update_controller_state(AppControllerInput *state, const PadState *pad,
    const HidSixAxisSensorHandle handles[MOTION_HANDLES], uint64_t *buttons)
{
    app_controller_neutral(state);
    state->connected = padIsConnected(pad);
    *buttons = padGetButtons(pad);
    const uint64_t switch_buttons[16] = {
        HidNpadButton_Left, HidNpadButton_Down, HidNpadButton_Right,
        HidNpadButton_Up, HidNpadButton_Plus, HidNpadButton_StickR,
        HidNpadButton_StickL, HidNpadButton_Minus,
        HidNpadButton_Y, HidNpadButton_B, HidNpadButton_A,
        HidNpadButton_X, HidNpadButton_R, HidNpadButton_L,
        HidNpadButton_ZR, HidNpadButton_ZL,
    };
    for (unsigned i = 0; i < 16; ++i) {
        if (*buttons & switch_buttons[i])
            state->buttons |= 1ULL << i;
    }
    state->left_x = padGetStickPos(pad, 0).x;
    state->left_y = padGetStickPos(pad, 0).y;
    state->right_x = padGetStickPos(pad, 1).x;
    state->right_y = padGetStickPos(pad, 1).y;
    state->left_trigger = (*buttons & HidNpadButton_ZL) ? 32767 : 0;
    state->right_trigger = (*buttons & HidNpadButton_ZR) ? 32767 : 0;

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

AppPlatform *app_platform_create(void)
{
    AppPlatform *platform = calloc(1, sizeof(*platform));
    if (!platform)
        return NULL;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        free(platform);
        return NULL;
    }
    platform->display = sdl_display_create();
    if (!platform->display) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        free(platform);
        return NULL;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&platform->pad);
    initialize_motion(platform->motion_handles);
    strcpy(platform->ip_address, "No network");
    return platform;
}

void app_platform_destroy(AppPlatform *platform)
{
    if (!platform)
        return;
    switch_av_destroy(platform->media);
    sdl_display_destroy(platform->display);
    for (unsigned i = 0; i < MOTION_HANDLES; ++i) {
        if (platform->motion_handles[i].type_value != 0)
            hidStopSixAxisSensor(platform->motion_handles[i]);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(platform);
}

bool app_platform_start_network(AppPlatform *platform)
{
    Result nifm_result = nifmInitialize(NifmServiceType_User);
    platform->nifm_initialized = R_SUCCEEDED(nifm_result);
    if (platform->nifm_initialized) {
        uint32_t current_ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&current_ip))) {
            struct in_addr address = {.s_addr = current_ip};
            inet_ntop(AF_INET, &address, platform->ip_address,
                sizeof(platform->ip_address));
        }
    }
    platform->socket_initialized =
        R_SUCCEEDED(socketInitializeDefault());
    return platform->socket_initialized;
}

void app_platform_stop_network(AppPlatform *platform)
{
    if (platform->socket_initialized)
        socketExit();
    if (platform->nifm_initialized)
        nifmExit();
}

bool app_platform_server_config(AppPlatform *platform,
    DsuServerConfig *config)
{
    uint32_t server_id = (uint32_t)svcGetSystemTick();
    platform->config = (DsuServerConfig){
        .server_id = server_id,
        .controller_mac = {
            0x02, 0x00, 0x44, (uint8_t)(server_id >> 16),
            (uint8_t)(server_id >> 8), (uint8_t)server_id,
        },
    };
    *config = platform->config;
    return true;
}

bool app_platform_attach_server(AppPlatform *platform, DsuServer *server)
{
    platform->media = switch_av_create(server, platform->display);
    return platform->media != NULL;
}

bool app_platform_running(AppPlatform *platform)
{
    if (!appletMainLoop())
        return false;
    if (!sdl_display_process_events(platform->display))
        return false;
    padUpdate(&platform->pad);
    uint64_t buttons = padGetButtons(&platform->pad);
    uint64_t now_us = clock_us();
    bool exit_buttons_held =
        (buttons & (HidNpadButton_Plus | HidNpadButton_Minus)) ==
        (HidNpadButton_Plus | HidNpadButton_Minus);
    if (app_hold_update(&platform->exit_hold, exit_buttons_held, now_us,
            EXIT_HOLD_US))
        return false;
    return true;
}

const char *app_platform_ip_address(const AppPlatform *platform)
{
    return platform->ip_address;
}

const char *app_platform_name(const AppPlatform *platform)
{
    (void)platform;
    return DSU_SWITCH_PLATFORM_NAME;
}

void app_platform_update_controller(AppPlatform *platform,
    AppControllerInput *controller)
{
    uint64_t buttons_down = padGetButtonsDown(&platform->pad);
    switch_av_handle_input(platform->media, buttons_down);
    update_controller_state(controller, &platform->pad,
        platform->motion_handles, &platform->controller_buttons);
}

const DsuAvCapabilities *app_platform_av_capabilities(
    const AppPlatform *platform)
{
    return switch_av_capabilities(platform->media);
}

const DsuAvCallbacks *app_platform_av_callbacks(const AppPlatform *platform)
{
    return switch_av_callbacks(platform->media);
}

void *app_platform_av_context(AppPlatform *platform)
{
    return platform->media;
}

void app_platform_poll_media(AppPlatform *platform)
{
    switch_av_poll(platform->media);
}

void app_platform_render_status(AppPlatform *platform, const DsuServer *server,
    const DsuControllerState *controller)
{
    (void)platform->frame++;
    sdl_display_status(platform->display, server, controller,
        DSU_SWITCH_PLATFORM_NAME, platform->ip_address,
        platform->controller_buttons,
        "HOLD A TO ENABLE OR MUTE");
}

void app_platform_sleep(AppPlatform *platform)
{
    (void)platform;
    svcSleepThread(SEND_INTERVAL_NS);
}
