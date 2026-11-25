#include <dsu/dsu_server.h>
#include <av/av_codecs.h>
#include "../app/platform.h"
#include "../app/sdl_display.h"

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
#define MAX_OPUS_SAMPLES 2880
#define AUDIO_QUEUE_LIMIT (48000 * 2 * 2 / 5)

typedef struct {
    AvCodecs *codecs;
    DsuAvSessionSettings settings;
    SdlDisplay *display;
    SDL_AudioDeviceID audio_device;
    int16_t pcm[MAX_OPUS_SAMPLES * 2];
    bool active;
} DesktopAv;

struct AppPlatform {
    SdlDisplay *display;
    SDL_GameController *controller;
    DesktopAv av;
    DsuAvCapabilities av_capabilities;
    DsuAvCallbacks av_callbacks;
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

static uint8_t axis_to_dsu(int32_t value)
{
    if (value < -32768)
        value = -32768;
    if (value > 32767)
        value = 32767;
    return (uint8_t)((value + 32768) * 255 / 65535);
}

static uint8_t trigger_to_dsu(int16_t value)
{
    return (uint8_t)((int32_t)value * 255 / 32767);
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

static void update_controller_state(DsuControllerState *state,
    SDL_GameController *controller)
{
    memset(state, 0, sizeof(*state));
    state->connected = true;
    state->left_stick_x = 128;
    state->left_stick_y = 128;
    state->right_stick_x = 128;
    state->right_stick_y = 128;
    if (!controller)
        return;

    const bool first[8] = {
        controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT),
        controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
        controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
        controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_UP),
        controller_button(controller, SDL_CONTROLLER_BUTTON_START),
        controller_button(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK),
        controller_button(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK),
        controller_button(controller, SDL_CONTROLLER_BUTTON_BACK),
    };
    const bool second[8] = {
        controller_button(controller, SDL_CONTROLLER_BUTTON_X),
        controller_button(controller, SDL_CONTROLLER_BUTTON_A),
        controller_button(controller, SDL_CONTROLLER_BUTTON_B),
        controller_button(controller, SDL_CONTROLLER_BUTTON_Y),
        controller_button(controller,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
        controller_button(controller,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER),
        false,
        false,
    };
    for (unsigned i = 0; i < 8; ++i) {
        if (first[i])
            state->button_masks[0] |= (uint8_t)(0x80U >> i);
        if (second[i])
            state->button_masks[1] |= (uint8_t)(0x80U >> i);
    }

    state->home_button =
        controller_button(controller, SDL_CONTROLLER_BUTTON_GUIDE);
    state->left_stick_x = axis_to_dsu(SDL_GameControllerGetAxis(controller,
        SDL_CONTROLLER_AXIS_LEFTX));
    state->left_stick_y = axis_to_dsu(-SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_LEFTY));
    state->right_stick_x = axis_to_dsu(SDL_GameControllerGetAxis(controller,
        SDL_CONTROLLER_AXIS_RIGHTX));
    state->right_stick_y = axis_to_dsu(-SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_RIGHTY));

    state->analog_buttons[0] = first[0] ? 255 : 0;
    state->analog_buttons[1] = first[1] ? 255 : 0;
    state->analog_buttons[2] = first[2] ? 255 : 0;
    state->analog_buttons[3] = first[3] ? 255 : 0;
    for (unsigned i = 0; i < 6; ++i)
        state->analog_buttons[4 + i] = second[i] ? 255 : 0;
    state->analog_buttons[10] = trigger_to_dsu(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    state->analog_buttons[11] = trigger_to_dsu(SDL_GameControllerGetAxis(
        controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));

    if (state->analog_buttons[10] > 0)
        state->button_masks[1] |= 0x02;
    if (state->analog_buttons[11] > 0)
        state->button_masks[1] |= 0x01;
}

static bool av_session_start(void *context,
    const DsuAvSessionSettings *settings)
{
    DesktopAv *av = context;
    if (av->active)
        return false;
    av->settings = *settings;
    av->codecs = av_codecs_create(settings);
    if (!av->codecs)
        return false;

    if (settings->audio_codec != DSU_AV_CODEC_NONE) {
        SDL_AudioSpec desired = {
            .freq = settings->audio_sample_rate_div100 * 100,
            .format = AUDIO_S16SYS,
            .channels = settings->audio_channels,
            .samples = 256,
        };
        SDL_AudioSpec obtained;
        av->audio_device = SDL_OpenAudioDevice(
            NULL, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (!av->audio_device || obtained.freq != desired.freq ||
            obtained.channels != desired.channels) {
            av_codecs_destroy(av->codecs);
            av->codecs = NULL;
            return false;
        }
        SDL_PauseAudioDevice(av->audio_device, 0);
    }
    av->active = true;
    SDL_Log("DSU-AV session started: %ux%u @ %u FPS",
        settings->video_width, settings->video_height, settings->video_fps);
    return true;
}

static void av_session_stop(void *context)
{
    DesktopAv *av = context;
    sdl_display_clear_video(av->display);
    if (av->audio_device) {
        SDL_CloseAudioDevice(av->audio_device);
        av->audio_device = 0;
    }
    av_codecs_destroy(av->codecs);
    av->codecs = NULL;
    av->active = false;
}

static void video_frame_callback(void *context,
    const AvDecodedFrame *frame)
{
    DesktopAv *av = context;
    sdl_display_frame(av->display, av->codecs, frame);
}

static void av_video_frame(void *context, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint32_t length)
{
    DesktopAv *av = context;
    (void)flags;
    av_codecs_submit_h264(av->codecs, data, length, timestamp_us);
}

static void av_audio_packet(void *context, uint32_t sequence,
    uint64_t timestamp_us, uint32_t packet_id, uint8_t flags,
    const uint8_t *data, uint16_t length)
{
    (void)sequence;
    (void)timestamp_us;
    (void)packet_id;
    DesktopAv *av = context;
    if (!av->audio_device)
        return;
    int samples = av_codecs_decode_opus(av->codecs, data, length,
        (flags & 0x01) != 0, av->pcm, MAX_OPUS_SAMPLES);
    if (samples <= 0)
        return;
    if ((uint32_t)SDL_GetQueuedAudioSize(av->audio_device) >
        AUDIO_QUEUE_LIMIT)
        SDL_ClearQueuedAudio(av->audio_device);
    SDL_QueueAudio(av->audio_device, av->pcm,
        (Uint32)(samples * av->settings.audio_channels * sizeof(int16_t)));
}

static void av_poll(DesktopAv *av)
{
    if (av->active)
        av_codecs_poll_video(av->codecs, video_frame_callback, av);
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
    platform->av.display = platform->display;

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
    platform->av_capabilities = (DsuAvCapabilities){
        .flags = DSU_AV_CAP_VIDEO | DSU_AV_CAP_AUDIO |
            DSU_AV_CAP_KEYFRAME | DSU_AV_CAP_QUALITY,
        .video_codec = DSU_AV_CODEC_H264,
        .audio_codec = DSU_AV_CODEC_OPUS,
        .video_width = 1280,
        .video_height = 720,
        .video_fps = 60,
        .video_bitrate_kbps = 8000,
        .audio_sample_rate_div100 = 480,
        .audio_channels = 2,
        .audio_frame_duration_01ms = 100,
        .payload_limit = 1100,
    };
    platform->av_callbacks = (DsuAvCallbacks){
        .session_start = av_session_start,
        .session_stop = av_session_stop,
        .video_frame = av_video_frame,
        .audio_packet = av_audio_packet,
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
    av_session_stop(&platform->av);
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
    DsuControllerState *controller)
{
    SDL_GameControllerUpdate();
    update_controller_state(controller, platform->controller);
}

const DsuAvCapabilities *app_platform_av_capabilities(
    const AppPlatform *platform)
{
    return &platform->av_capabilities;
}

const DsuAvCallbacks *app_platform_av_callbacks(const AppPlatform *platform)
{
    return &platform->av_callbacks;
}

void *app_platform_av_context(AppPlatform *platform)
{
    return &platform->av;
}

void app_platform_poll_media(AppPlatform *platform)
{
    av_poll(&platform->av);
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
