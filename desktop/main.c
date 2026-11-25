#include <dsu/dsu_server.h>
#include <av/av_codecs.h>

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
    SDL_Window *video_window;
    SDL_Renderer *video_renderer;
    SDL_Texture *video_texture;
    SDL_AudioDeviceID audio_device;
    int16_t pcm[MAX_OPUS_SAMPLES * 2];
    bool active;
} DesktopAv;

static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
        (uint64_t)now.tv_nsec / 1000ULL;
}

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

static void render_controller(SDL_Renderer *renderer,
    const DsuControllerState *state)
{
    SDL_SetRenderDrawColor(renderer, 24, 27, 32, 255);
    SDL_RenderClear(renderer);

    const int stick_y = WINDOW_HEIGHT / 2;
    const int stick_centers[2] = {150, 340};
    for (unsigned i = 0; i < 2; ++i) {
        SDL_SetRenderDrawColor(renderer, 72, 78, 90, 255);
        SDL_Rect area = {stick_centers[i] - 55, stick_y - 55, 110, 110};
        SDL_RenderDrawRect(renderer, &area);
    }

    int left_x = stick_centers[0] +
        ((int)state->left_stick_x - 128) * 45 / 128;
    int left_y = stick_y -
        ((int)state->left_stick_y - 128) * 45 / 128;
    int right_x = stick_centers[1] +
        ((int)state->right_stick_x - 128) * 45 / 128;
    int right_y = stick_y -
        ((int)state->right_stick_y - 128) * 45 / 128;
    SDL_SetRenderDrawColor(renderer, state->connected ? 70 : 160,
        state->connected ? 190 : 70, 100, 255);
    SDL_Rect left = {left_x - 10, left_y - 10, 20, 20};
    SDL_Rect right = {right_x - 10, right_y - 10, 20, 20};
    SDL_RenderFillRect(renderer, &left);
    SDL_RenderFillRect(renderer, &right);

    const int button_x[4] = {545, 580, 615, 580};
    const int button_y[4] = {120, 85, 120, 155};
    const uint8_t analog_indexes[4] = {7, 6, 5, 4};
    for (unsigned i = 0; i < 4; ++i) {
        bool pressed = state->analog_buttons[analog_indexes[i]] != 0;
        SDL_SetRenderDrawColor(renderer, pressed ? 255 : 100,
            pressed ? 210 : 105, pressed ? 60 : 115, 255);
        SDL_Rect button = {button_x[i] - 14, button_y[i] - 14, 28, 28};
        SDL_RenderFillRect(renderer, &button);
    }
    SDL_RenderPresent(renderer);
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

static void close_video_window(DesktopAv *av)
{
    if (av->video_texture) {
        SDL_DestroyTexture(av->video_texture);
        av->video_texture = NULL;
    }
    if (av->video_renderer) {
        SDL_DestroyRenderer(av->video_renderer);
        av->video_renderer = NULL;
    }
    if (av->video_window) {
        SDL_DestroyWindow(av->video_window);
        av->video_window = NULL;
    }
}

static void av_session_stop(void *context)
{
    DesktopAv *av = context;
    close_video_window(av);
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
    if (!av->video_window) {
        av->video_window = SDL_CreateWindow("DSU-AV Video",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            frame->width, frame->height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
        if (av->video_window)
            av->video_renderer = SDL_CreateRenderer(av->video_window, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!av->video_renderer && av->video_window)
            av->video_renderer =
                SDL_CreateRenderer(av->video_window, -1, SDL_RENDERER_SOFTWARE);
        if (av->video_renderer)
            av->video_texture = SDL_CreateTexture(av->video_renderer,
                SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                frame->width, frame->height);
        if (!av->video_texture) {
            close_video_window(av);
            return;
        }
    }

    uint8_t *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(av->video_texture, NULL, (void **)&pixels, &pitch) < 0)
        return;
    if (!av_codecs_scale_to_rgba(av->codecs, frame, pixels, pitch,
            frame->width, frame->height))
        memset(pixels, 0, (size_t)pitch * frame->height);
    SDL_UnlockTexture(av->video_texture);
    SDL_RenderClear(av->video_renderer);
    SDL_RenderCopy(av->video_renderer, av->video_texture, NULL, NULL);
    SDL_RenderPresent(av->video_renderer);
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
            SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("DSU Desktop Controller",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("SDL window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_Log("SDL renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    char address[INET_ADDRSTRLEN] = "127.0.0.1";
    find_local_ipv4(address, sizeof(address));
    uint32_t server_id = (uint32_t)(SDL_GetPerformanceCounter() ^
        (uint64_t)time(NULL));
    DsuServerConfig config = {
        .server_id = server_id,
        .controller_mac = {
            0x02, 0x44, 0x53, (uint8_t)(server_id >> 16),
            (uint8_t)(server_id >> 8), (uint8_t)server_id,
        },
    };
    DsuServer server = {0};
    if (!dsu_server_init(&server, &config)) {
        SDL_Log("Could not bind DSU UDP port %d", DSU_PORT);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    DesktopAv av = {0};
    DsuAvCapabilities av_capabilities = {
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
    DsuAvCallbacks av_callbacks = {
        .session_start = av_session_start,
        .session_stop = av_session_stop,
        .video_frame = av_video_frame,
        .audio_packet = av_audio_packet,
    };
    dsu_server_configure_av(&server, &av_capabilities, &av_callbacks, &av);
    SDL_Log("DSU server listening on %s:%d", address, DSU_PORT);

    SDL_GameController *controller = open_controller();
    SDL_Log("Controller: %s",
        controller ? SDL_GameControllerName(controller) :
            "waiting for an SDL-mapped gamepad");
    DsuControllerState state = {0};
    bool running = true;
    uint64_t next_title_update = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
            else if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                av.video_window &&
                event.window.windowID == SDL_GetWindowID(av.video_window))
                close_video_window(&av);
            else if (event.type == SDL_CONTROLLERDEVICEADDED && !controller)
                controller = SDL_GameControllerOpen(event.cdevice.which);
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
                controller && event.cdevice.which ==
                    SDL_JoystickInstanceID(
                        SDL_GameControllerGetJoystick(controller))) {
                SDL_GameControllerClose(controller);
                controller = NULL;
            }
        }

        SDL_GameControllerUpdate();
        update_controller_state(&state, controller);
        dsu_server_poll(&server, &state);
        av_poll(&av);
        dsu_server_broadcast(&server, &state);
        dsu_server_expire_clients(&server);
        render_controller(renderer, &state);

        uint64_t now = monotonic_us();
        if (now >= next_title_update) {
            char title[160];
            snprintf(title, sizeof(title),
                "DSU Desktop Controller - %s:%d - %s - %u client(s)",
                address, DSU_PORT,
                controller ? SDL_GameControllerName(controller) :
                    "virtual controller (neutral)", server.subscriptions);
            SDL_SetWindowTitle(window, title);
            next_title_update = now + 1000000;
        }
        SDL_Delay(POLL_INTERVAL_MS);
    }

    if (controller)
        SDL_GameControllerClose(controller);
    av_session_stop(&av);
    dsu_server_close(&server);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
