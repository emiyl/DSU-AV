#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>
#include <av/av_codecs.h>
#include <dsu/dsu_server.h>

typedef struct SdlDisplay SdlDisplay;

typedef struct {
    uint64_t submitted_us;
    uint64_t decoded_us;
    uint64_t uploaded_us;
    uint64_t presented_us;
    uint64_t frame_gap_us;
    uint64_t callback_us;
    uint32_t frames;
    uint32_t keyframes;
    uint32_t video_packets;
    uint32_t video_lost;
} SdlVideoDiagnostics;

SdlDisplay *sdl_display_create(void);
void sdl_display_destroy(SdlDisplay *display);
void sdl_display_frame(SdlDisplay *display, AvCodecs *codecs,
    const AvDecodedFrame *frame);
void sdl_display_status(SdlDisplay *display, const DsuServer *server,
    const DsuControllerState *controller, const char *platform_name,
    const char *ip_address, uint64_t raw_buttons,
    const char *microphone_status);
bool sdl_display_has_video(const SdlDisplay *display);
void sdl_display_clear_video(SdlDisplay *display);
bool sdl_display_process_events(SdlDisplay *display);
void sdl_display_get_touch(SdlDisplay *display, DsuTouchPoint touch[2]);
void sdl_display_set_video_diagnostics(SdlDisplay *display,
    const SdlVideoDiagnostics *diagnostics);
