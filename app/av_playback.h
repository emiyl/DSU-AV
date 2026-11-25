#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dsu/dsu_server.h>
#include <av/av_codecs.h>

#include "sdl_display.h"

typedef struct AppAvPlayback AppAvPlayback;

typedef struct {
    void *context;
    bool (*video_start)(void *context,
        const DsuAvSessionSettings *settings);
    void (*video_stop)(void *context);
    bool (*video_submit)(void *context, const uint8_t *data,
        uint32_t length, uint64_t timestamp_us);
    void (*video_poll)(void *context, AvDecodedFrameCallback callback,
        void *callback_context);
} AppAvVideoBackend;

AppAvPlayback *app_av_playback_create(SdlDisplay *display,
    bool microphone_supported, const AppAvVideoBackend *video_backend);
void app_av_playback_destroy(AppAvPlayback *playback);
const DsuAvCapabilities *app_av_playback_capabilities(
    const AppAvPlayback *playback);
const DsuAvCallbacks *app_av_playback_callbacks(
    const AppAvPlayback *playback);
bool app_av_playback_active(const AppAvPlayback *playback);
void app_av_playback_poll(AppAvPlayback *playback);
AvCodecs *app_av_playback_codecs(AppAvPlayback *playback);
const DsuAvSessionSettings *app_av_playback_settings(
    const AppAvPlayback *playback);
