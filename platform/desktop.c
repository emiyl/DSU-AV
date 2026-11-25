#include <dsu/dsu_server.h>
#include <av/av_codecs.h>
#include "../app/platform.h"
#include "../app/sdl_display.h"
#include "../app/av_playback.h"
#include "../app/controller.h"

#include <SDL.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 240
#define POLL_INTERVAL_MS 8
struct AppPlatform {
    SdlDisplay *display;
    SDL_GameController *controller;
    AppAvPlayback *av;
    DsuServerConfig config;
    char ip_address[INET_ADDRSTRLEN];
    bool running;
    uint64_t next_title_update_us;
};

static bool find_local_ipv4(char *output, size_t output_size)
{
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return false;

    bool found = false;
    for (struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) ||
            !(entry->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *address =
            (struct sockaddr_in *)entry->ifa_addr;
        if (address->sin_addr.s_addr == 0)
            continue;
        if (inet_ntop(AF_INET, &address->sin_addr, output, output_size)) {
            found = true;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

static bool controller_button(SDL_GameController *controller,
    SDL_GameControllerButton button)
{
    return SDL_GameControllerGetButton(controller, button) != 0;
}

static SDL_GameController *open_controller(void)
{
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index))
            continue;
        SDL_GameController *controller = SDL_GameControllerOpen(index);
        if (controller)
            return controller;
    }
    return NULL;
}

static void update_controller_state(AppControllerInput *state,
    SDL_GameController *controller)
{
    app_controller_neutral(state);
    if (!controller)
        return;

    const AppButton first[8] = {
        APP_BUTTON_DPAD_LEFT, APP_BUTTON_DPAD_DOWN,
        APP_BUTTON_DPAD_RIGHT, APP_BUTTON_DPAD_UP,
        APP_BUTTON_OPTIONS, APP_BUTTON_STICK_R,
        APP_BUTTON_STICK_L, APP_BUTTON_SHARE,
    };
    const SDL_GameControllerButton first_sdl[8] = {
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_BACK,
    };
    const AppButton second[8] = {
        APP_BUTTON_Y, APP_BUTTON_B, APP_BUTTON_A, APP_BUTTON_X,
        APP_BUTTON_R1, APP_BUTTON_L1, APP_BUTTON_R2, APP_BUTTON_L2,
    };
    const SDL_GameControllerButton second_sdl[8] = {
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_INVALID,
    };
    for (unsigned i = 0; i < 8; ++i) {
        if (first_sdl[i] != SDL_CONTROLLER_BUTTON_INVALID &&
            controller_button(controller, first_sdl[i]))
            state->buttons |= first[i];
        if (second_sdl[i] != SDL_CONTROLLER_BUTTON_INVALID &&
            controller_button(controller, second_sdl[i]))
            state->buttons |= second[i];
    }

    state->home =
        controller_button(controller, SDL_CONTROLLER_BUTTON_GUIDE);
    state->left_x = SDL_GameControllerGetAxis(controller,
        SDL_CONTROLLER_AXIS_LEFTX);
    state->left_y = -SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTY);
    state->right_x = SDL_GameControllerGetAxis(controller,
        SDL_CONTROLLER_AXIS_RIGHTX);
    state->right_y = -SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTY);
    state->right_trigger = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    state->left_trigger = SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
}

AppPlatform *app_platform_create(void)
{
    AppPlatform *platform = calloc(1, sizeof(*platform));
    if (!platform)
        return NULL;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
            SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        free(platform);
        return NULL;
    }

    platform->display = sdl_display_create();
    if (!platform->display) {
        SDL_Quit();
        free(platform);
        return NULL;
    }
    platform->av = app_av_playback_create(platform->display, false, NULL);
    if (!platform->av) {
        sdl_display_destroy(platform->display);
        SDL_Quit();
        free(platform);
        return NULL;
    }

    strcpy(platform->ip_address, "127.0.0.1");
    find_local_ipv4(platform->ip_address, sizeof(platform->ip_address));
    uint32_t server_id = (uint32_t)(SDL_GetPerformanceCounter() ^
        (uint64_t)time(NULL));
    platform->config = (DsuServerConfig){
        .server_id = server_id,
        .controller_mac = {
            0x02, 0x44, 0x53, (uint8_t)(server_id >> 16),
            (uint8_t)(server_id >> 8), (uint8_t)server_id,
        },
    };
    platform->controller = open_controller();
    SDL_Log("Controller: %s",
        platform->controller ? SDL_GameControllerName(platform->controller) :
            "waiting for an SDL-mapped gamepad");
    platform->running = true;
    return platform;
}

void app_platform_destroy(AppPlatform *platform)
{
    if (!platform)
        return;
    if (platform->controller)
        SDL_GameControllerClose(platform->controller);
    app_av_playback_destroy(platform->av);
    sdl_display_destroy(platform->display);
    SDL_Quit();
    free(platform);
}

bool app_platform_running(AppPlatform *platform)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            platform->running = false;
        else if (event.type == SDL_CONTROLLERDEVICEADDED &&
            !platform->controller)
            platform->controller =
                SDL_GameControllerOpen(event.cdevice.which);
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
            platform->controller &&
            event.cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(
                    platform->controller))) {
            SDL_GameControllerClose(platform->controller);
            platform->controller = NULL;
        }
    }
    if (!sdl_display_process_events(platform->display))
        platform->running = false;
    return platform->running;
}

bool app_platform_start_network(AppPlatform *platform)
{
    return platform != NULL;
}

void app_platform_stop_network(AppPlatform *platform)
{
    (void)platform;
}

bool app_platform_attach_server(AppPlatform *platform, DsuServer *server)
{
    (void)platform;
    (void)server;
    return true;
}

bool app_platform_server_config(AppPlatform *platform,
    DsuServerConfig *config)
{
    if (!platform || !config)
        return false;
    *config = platform->config;
    return true;
}

const char *app_platform_ip_address(const AppPlatform *platform)
{
    return platform->ip_address;
}

const char *app_platform_name(const AppPlatform *platform)
{
    (void)platform;
    return "Desktop SDL";
}

void app_platform_update_controller(AppPlatform *platform,
    AppControllerInput *controller)
{
    SDL_GameControllerUpdate();
    update_controller_state(controller, platform->controller);
}

const DsuAvCapabilities *app_platform_av_capabilities(
    const AppPlatform *platform)
{
    return app_av_playback_capabilities(platform->av);
}

const DsuAvCallbacks *app_platform_av_callbacks(const AppPlatform *platform)
{
    return app_av_playback_callbacks(platform->av);
}

void *app_platform_av_context(AppPlatform *platform)
{
    return platform->av;
}

void app_platform_poll_media(AppPlatform *platform)
{
    app_av_playback_poll(platform->av);
}

void app_platform_render_status(AppPlatform *platform, const DsuServer *server,
    const DsuControllerState *controller)
{
    sdl_display_status(platform->display, server, controller, "Desktop SDL",
        platform->ip_address,
        ((uint64_t)controller->button_masks[0] << 8) |
            controller->button_masks[1],
        "NOT AVAILABLE");
}

void app_platform_sleep(AppPlatform *platform)
{
    (void)platform;
    SDL_Delay(POLL_INTERVAL_MS);
}
