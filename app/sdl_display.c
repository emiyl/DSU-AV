#include "sdl_display.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define FONT_SCALE 3
#define FONT_WIDTH 6
#define FONT_HEIGHT 7

struct SdlDisplay {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *video_texture;
    int video_width;
    int video_height;
    Uint32 video_format;
    bool has_video;
    bool running;
    DsuTouchPoint touch[2];
    SdlVideoDiagnostics video_diagnostics;
    uint64_t video_present_start_us;
    uint64_t last_present_us;
};

static const uint8_t font_5x7[][7] = {
    [' '] = {0, 0, 0, 0, 0, 0, 0},
    ['-'] = {0, 0, 0, 31, 0, 0, 0},
    ['.'] = {0, 0, 0, 0, 0, 12, 12},
    [':'] = {0, 12, 12, 0, 12, 12, 0},
    ['+'] = {0, 4, 4, 31, 4, 4, 0},
    ['('] = {2, 4, 8, 8, 8, 4, 2},
    [')'] = {8, 4, 2, 2, 2, 4, 8},
    ['/'] = {1, 2, 4, 8, 16, 0, 0},
    ['0'] = {14, 17, 19, 21, 25, 17, 14},
    ['1'] = {4, 12, 4, 4, 4, 4, 14},
    ['2'] = {14, 17, 1, 2, 4, 8, 31},
    ['3'] = {30, 1, 1, 14, 1, 1, 30},
    ['4'] = {2, 6, 10, 18, 31, 2, 2},
    ['5'] = {31, 16, 16, 30, 1, 1, 30},
    ['6'] = {14, 16, 16, 30, 17, 17, 14},
    ['7'] = {31, 1, 2, 4, 8, 8, 8},
    ['8'] = {14, 17, 17, 14, 17, 17, 14},
    ['9'] = {14, 17, 17, 15, 1, 1, 14},
    ['A'] = {14, 17, 17, 31, 17, 17, 17},
    ['B'] = {30, 17, 17, 30, 17, 17, 30},
    ['C'] = {15, 16, 16, 16, 16, 16, 15},
    ['D'] = {30, 17, 17, 17, 17, 17, 30},
    ['E'] = {31, 16, 16, 30, 16, 16, 31},
    ['F'] = {31, 16, 16, 30, 16, 16, 16},
    ['G'] = {15, 16, 16, 23, 17, 17, 15},
    ['H'] = {17, 17, 17, 31, 17, 17, 17},
    ['I'] = {14, 4, 4, 4, 4, 4, 14},
    ['J'] = {1, 1, 1, 1, 17, 17, 14},
    ['K'] = {17, 18, 20, 24, 20, 18, 17},
    ['L'] = {16, 16, 16, 16, 16, 16, 31},
    ['M'] = {17, 27, 21, 21, 17, 17, 17},
    ['N'] = {17, 25, 21, 19, 17, 17, 17},
    ['O'] = {14, 17, 17, 17, 17, 17, 14},
    ['P'] = {30, 17, 17, 30, 16, 16, 16},
    ['Q'] = {14, 17, 17, 17, 21, 18, 13},
    ['R'] = {30, 17, 17, 30, 20, 18, 17},
    ['S'] = {15, 16, 16, 14, 1, 1, 30},
    ['T'] = {31, 4, 4, 4, 4, 4, 4},
    ['U'] = {17, 17, 17, 17, 17, 17, 14},
    ['V'] = {17, 17, 17, 17, 17, 10, 4},
    ['W'] = {17, 17, 17, 21, 21, 21, 10},
    ['X'] = {17, 17, 10, 4, 10, 17, 17},
    ['Y'] = {17, 17, 10, 4, 4, 4, 4},
    ['Z'] = {31, 1, 2, 4, 8, 16, 31},
};

static uint64_t clock_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
        (uint64_t)now.tv_nsec / 1000ULL;
}

SdlDisplay *sdl_display_create(void)
{
    SdlDisplay *display = calloc(1, sizeof(*display));
    if (!display)
        return NULL;
    display->running = true;
    display->window = SDL_CreateWindow("DSU Controller",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!display->window) {
        free(display);
        return NULL;
    }
    display->renderer = SDL_CreateRenderer(display->window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!display->renderer)
        display->renderer = SDL_CreateRenderer(
            display->window, -1, SDL_RENDERER_SOFTWARE);
    if (!display->renderer) {
        SDL_DestroyWindow(display->window);
        free(display);
        return NULL;
    }
    SDL_SetRenderDrawBlendMode(display->renderer, SDL_BLENDMODE_BLEND);
    return display;
}

void sdl_display_destroy(SdlDisplay *display)
{
    if (!display)
        return;
    SDL_DestroyTexture(display->video_texture);
    SDL_DestroyRenderer(display->renderer);
    SDL_DestroyWindow(display->window);
    free(display);
}

void sdl_display_frame(SdlDisplay *display, AvCodecs *codecs,
    const AvDecodedFrame *frame)
{
    if (!display || !frame)
        return;
    if (frame->format == AV_PIX_FMT_NV12) {
        if (display->video_width != frame->width ||
            display->video_height != frame->height ||
            display->video_format != SDL_PIXELFORMAT_NV12 ||
            !display->video_texture) {
            SDL_DestroyTexture(display->video_texture);
            display->video_texture = SDL_CreateTexture(display->renderer,
                SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING,
                frame->width, frame->height);
            display->video_width = frame->width;
            display->video_height = frame->height;
            display->video_format = SDL_PIXELFORMAT_NV12;
        }
        if (!display->video_texture)
            return;
        uint64_t upload_start_us = clock_us();
        if (SDL_UpdateNVTexture(display->video_texture, NULL,
                frame->planes[0], frame->strides[0],
                frame->planes[1], frame->strides[1]) < 0)
            return;
        display->video_diagnostics.uploaded_us =
            clock_us() - upload_start_us;
        display->has_video = true;
        return;
    }
    if (display->video_width != frame->width ||
        display->video_height != frame->height ||
        display->video_format != SDL_PIXELFORMAT_ABGR8888 ||
        !display->video_texture) {
        SDL_DestroyTexture(display->video_texture);
        display->video_texture = SDL_CreateTexture(display->renderer,
            SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
            frame->width, frame->height);
        display->video_width = frame->width;
        display->video_height = frame->height;
        display->video_format = SDL_PIXELFORMAT_ABGR8888;
    }
    if (!display->video_texture)
        return;
    uint8_t *pixels;
    int pitch;
    if (SDL_LockTexture(display->video_texture, NULL,
            (void **)&pixels, &pitch) < 0)
        return;
    bool scaled = av_codecs_scale_to_rgba(codecs, frame, pixels, pitch,
        frame->width, frame->height);
    SDL_UnlockTexture(display->video_texture);
    if (scaled)
        display->has_video = true;
}

static void draw_text(SDL_Renderer *renderer, int x, int y,
    const char *text)
{
    SDL_SetRenderDrawColor(renderer, 245, 247, 250, 255);
    for (size_t i = 0; text[i]; ++i) {
        unsigned char character = (unsigned char)toupper(
            (unsigned char)text[i]);
        if (character >= 128)
            character = '?';
        const uint8_t *glyph = font_5x7[character];
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (!(glyph[row] & (1U << (4 - column))))
                    continue;
                SDL_Rect pixel = {
                    x + (int)(i * FONT_WIDTH + column) * FONT_SCALE,
                    y + row * FONT_SCALE,
                    FONT_SCALE, FONT_SCALE,
                };
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
    }
}

void sdl_display_set_video_diagnostics(SdlDisplay *display,
    const SdlVideoDiagnostics *diagnostics)
{
    if (!display || !diagnostics)
        return;
    display->video_diagnostics.submitted_us = diagnostics->submitted_us;
    display->video_diagnostics.decoded_us = diagnostics->decoded_us;
    display->video_diagnostics.callback_us = diagnostics->callback_us;
    display->video_diagnostics.frame_gap_us = diagnostics->frame_gap_us;
    display->video_diagnostics.frames = diagnostics->frames;
    display->video_diagnostics.keyframes = diagnostics->keyframes;
    display->video_diagnostics.video_packets = diagnostics->video_packets;
    display->video_diagnostics.video_lost = diagnostics->video_lost;
}

static void draw_video_diagnostics(SdlDisplay *display)
{
    const SdlVideoDiagnostics *timings = &display->video_diagnostics;
    char lines[10][160];
    snprintf(lines[0], sizeof(lines[0]), "DSU-AV LATENCY DIAGNOSTICS");
    snprintf(lines[1], sizeof(lines[1]), "FRAME GAP  %6" PRIu64 " us",
        timings->frame_gap_us);
    snprintf(lines[2], sizeof(lines[2]), "SUBMIT     %6" PRIu64 " us",
        timings->submitted_us);
    snprintf(lines[3], sizeof(lines[3]), "DECODE     %6" PRIu64 " us",
        timings->decoded_us);
    snprintf(lines[4], sizeof(lines[4]), "NV12 UPLOAD%6" PRIu64 " us",
        timings->uploaded_us);
    snprintf(lines[5], sizeof(lines[5]), "PRESENT    %6" PRIu64 " us",
        timings->presented_us);
    snprintf(lines[6], sizeof(lines[6]), "RECEIVE    %6" PRIu64 " us",
        timings->callback_us);
    snprintf(lines[7], sizeof(lines[7]), "FRAMES     %6" PRIu32,
        timings->frames);
    snprintf(lines[8], sizeof(lines[8]), "KEYFRAMES  %6" PRIu32,
        timings->keyframes);
    snprintf(lines[9], sizeof(lines[9]), "VIDEO      %6" PRIu32 " PACK / %4" PRIu32 " LOST",
        timings->video_packets, timings->video_lost);

    SDL_SetRenderDrawBlendMode(display->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(display->renderer, 8, 10, 14, 210);
    SDL_Rect panel = {24, 24, 820, 390};
    SDL_RenderFillRect(display->renderer, &panel);
    for (unsigned i = 0; i < 10; ++i)
        draw_text(display->renderer, 42, 42 + (int)i * 34, lines[i]);
}

void sdl_display_status(SdlDisplay *display, const DsuServer *server,
    const DsuControllerState *controller, const char *platform_name,
    const char *ip_address, uint64_t raw_buttons,
    const char *microphone_status)
{
    if (!display)
        return;
    const DsuAvSessionSettings *av_settings =
        dsu_server_av_settings(server);
    bool video_stream_present = av_settings &&
        av_settings->video_codec != DSU_AV_CODEC_NONE;

    SDL_SetRenderDrawColor(display->renderer, 20, 23, 29, 255);
    if (video_stream_present) {
        SDL_RenderClear(display->renderer);
        if (display->has_video)
            SDL_RenderCopy(display->renderer, display->video_texture,
                NULL, NULL);
        draw_video_diagnostics(display);
        uint32_t fps = av_settings->video_fps ? av_settings->video_fps : 30;
        uint64_t present_interval_us = 1000000ULL / fps;
        uint64_t now_us = clock_us();
        if (display->last_present_us &&
            now_us - display->last_present_us < present_interval_us)
            return;
        uint64_t present_start_us = clock_us();
        SDL_RenderPresent(display->renderer);
        display->video_diagnostics.presented_us =
            clock_us() - present_start_us;
        display->last_present_us = now_us;
        return;
    }

    SDL_RenderClear(display->renderer);

    SDL_SetRenderDrawColor(display->renderer, 8, 10, 14, 220);
    SDL_Rect panel = {24, 24, 820, 330};
    SDL_RenderFillRect(display->renderer, &panel);

    char lines[8][160];
    snprintf(lines[0], sizeof(lines[0]), "DSU CONTROLLER SERVER");
    snprintf(lines[1], sizeof(lines[1]), "PLATFORM : %s", platform_name);
    snprintf(lines[2], sizeof(lines[2]), "LISTEN   : %s:%u",
        ip_address, (unsigned)DSU_PORT);
    snprintf(lines[3], sizeof(lines[3]), "CLIENT   : %s",
        server->subscriptions ? server->last_client :
            "WAITING FOR SUBSCRIPTION");
    snprintf(lines[4], sizeof(lines[4]), "INPUTS   : %s",
        controller->connected ? "CONTROLLER CONNECTED" :
            "CONTROLLER DISCONNECTED");
    snprintf(lines[5], sizeof(lines[5]),
        "DSU-AV   : %s - MICROPHONE %s",
        dsu_server_av_is_active(server) ? "SESSION ACTIVE" : "READY",
        microphone_status);
    snprintf(lines[6], sizeof(lines[6]),
        "SENT     : %" PRIu32 "  BUTTONS : %016" PRIx64,
        server->packets_sent, raw_buttons);
    snprintf(lines[7], sizeof(lines[7]), "L %3u,%3u  R %3u,%3u",
        controller->left_stick_x, controller->left_stick_y,
        controller->right_stick_x, controller->right_stick_y);
    for (unsigned i = 0; i < 8; ++i)
        draw_text(display->renderer, 42, 42 + (int)i * 38, lines[i]);
    SDL_RenderPresent(display->renderer);
}

bool sdl_display_has_video(const SdlDisplay *display)
{
    return display && display->has_video;
}

void sdl_display_clear_video(SdlDisplay *display)
{
    if (!display)
        return;
    SDL_DestroyTexture(display->video_texture);
    display->video_texture = NULL;
    display->video_width = 0;
    display->video_height = 0;
    display->video_format = 0;
    display->has_video = false;
    display->last_present_us = 0;
}

bool sdl_display_process_events(SdlDisplay *display)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            display->running = false;
        if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION) {
            const uint8_t id = (uint8_t)(event.tfinger.fingerId & 0xff);
            int slot = 0;
            while (slot < 2 && display->touch[slot].active &&
                display->touch[slot].id != id)
                ++slot;
            if (slot < 2) {
                display->touch[slot] = (DsuTouchPoint) {
                    .active = true,
                    .id = id,
                    .x = (uint16_t)(event.tfinger.x * 1920),
                    .y = (uint16_t)(event.tfinger.y * 942),
                };
            }
        } else if (event.type == SDL_FINGERUP) {
            const uint8_t id = (uint8_t)(event.tfinger.fingerId & 0xff);
            for (unsigned i = 0; i < 2; ++i)
                if (display->touch[i].id == id)
                    display->touch[i].active = false;
        }
    }
    return display->running;
}

void sdl_display_get_touch(SdlDisplay *display, DsuTouchPoint touch[2])
{
    memcpy(touch, display->touch, sizeof(display->touch));
}
