#pragma once

#include <stdbool.h>
#include <dsu/dsu_server.h>
#include "../app/sdl_display.h"

typedef struct SwitchAv SwitchAv;

SwitchAv *switch_av_create(DsuServer *server, SdlDisplay *display);
void switch_av_destroy(SwitchAv *media);
const DsuAvCapabilities *switch_av_capabilities(const SwitchAv *media);
const DsuAvCallbacks *switch_av_callbacks(const SwitchAv *media);
void switch_av_poll(SwitchAv *media);
bool switch_av_is_video_visible(const SwitchAv *media);
void switch_av_handle_input(SwitchAv *media, uint64_t buttons_down);
