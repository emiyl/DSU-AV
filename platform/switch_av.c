#include "switch_av.h"
#include <av/av_codecs.h>
#include "../app/av_playback.h"
#include "../app/microphone.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <switch.h>

#ifdef __SWITCH2__
#include <switch2/video/decoder.h>
#endif

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define MAX_ENCODED_FRAME (1024 * 1024)
#define MAX_OPUS_SAMPLES 2880
#define MICROPHONE_QUEUE_LIMIT (48000 * 2 / 5)

struct SwitchAv {
    DsuServer *server;
    AppAvPlayback *playback;
    DsuAvCallbacks callbacks;
    bool active;
    SdlDisplay *display;
    AppMicrophone microphone;

    SDL_AudioDeviceID capture_device;
    uint8_t opus_output[1500];

#ifdef __SWITCH2__
    VideoDecoder decoder;
    bool decoder_created;
    uint8_t codec_config[8192];
    uint32_t codec_config_size;
#endif
};

#ifdef __SWITCH2__
static bool decode_hardware_video(SwitchAv *media, const uint8_t *data,
    uint32_t length, uint64_t timestamp_us)
{
    if (!media->decoder_created) {
        if (!av_codecs_extract_h264_config(data, length, media->codec_config,
                sizeof(media->codec_config), &media->codec_config_size))
            return false;
        for (uint32_t i = 0; i + 3 < media->codec_config_size; ++i) {
            if (media->codec_config[i] == 0 && media->codec_config[i + 1] == 0 &&
                media->codec_config[i + 2] == 1 &&
                (i == 0 || media->codec_config[i - 1] != 0)) {
                if (media->codec_config_size > sizeof(media->codec_config) - 1)
                    return false;
                memmove(media->codec_config + i + 1,
                    media->codec_config + i, media->codec_config_size - i);
                ++media->codec_config_size;
                i += 3;
            }
        }
        VideoDecoderConfig config = {
            .codec = ViddecCodec_H264,
            .width = app_av_playback_settings(media->playback)->video_width,
            .height = app_av_playback_settings(media->playback)->video_height,
            .frame_rate = app_av_playback_settings(media->playback)->video_fps,
            .tiling = ViddecTilingMode_PitchLinear,
            .max_sample_size = MAX_ENCODED_FRAME,
            .codec_config = media->codec_config,
            .codec_config_size = media->codec_config_size,
        };
        if (R_FAILED(videoDecoderCreate(&media->decoder, &config)))
            return false;
        media->decoder_created = true;
    }

    if (R_FAILED(videoDecoderSubmit(&media->decoder, data, length,
            (s64)timestamp_us * 1000, false, 0))) {
        dsu_server_av_request_keyframe(media->server, 0, 1, 2);
        return false;
    }

    VideoFrame frame;
    while (R_SUCCEEDED(videoDecoderGetFrame(&media->decoder, &frame, 0))) {
        AvDecodedFrame decoded = {
            .timestamp_us = (uint64_t)frame.timestamp_ns / 1000,
            .planes = (const uint8_t *const[]){frame.planes[0],
                frame.planes[1], NULL, NULL},
            .strides = (const int[]){(int)frame.pitches[0],
                (int)frame.pitches[1], 0, 0},
            .format = AV_PIX_FMT_NV12,
            .width = (int)frame.width,
            .height = (int)frame.height,
        };
        sdl_display_frame(media->display,
            app_av_playback_codecs(media->playback), &decoded);
        videoDecoderReleaseFrame(&media->decoder, &frame);
    }
    return true;
}

static bool hardware_video_submit(void *context, const uint8_t *data,
    uint32_t length, uint64_t timestamp_us)
{
    SwitchAv *media = context;
    if (length > MAX_ENCODED_FRAME)
        return false;
    bool accepted = decode_hardware_video(media, data, length, timestamp_us);
    if (!accepted)
        dsu_server_av_request_keyframe(media->server, 0, 1, 2);
    return accepted;
}
#endif

static void close_media_session(SwitchAv *media)
{
    sdl_display_clear_video(media->display);
    if (media->capture_device) {
        SDL_PauseAudioDevice(media->capture_device, 1);
        SDL_CloseAudioDevice(media->capture_device);
        media->capture_device = 0;
    }
#ifdef __SWITCH2__
    if (media->decoder_created) {
        videoDecoderClose(&media->decoder);
        media->decoder_created = false;
    }
#endif
    media->active = false;
    app_microphone_session_stop(&media->microphone);
}

static void receive_video(void *context, uint64_t timestamp_us, uint8_t flags,
    const uint8_t *data, uint32_t length)
{
    SwitchAv *media = context;
#ifdef __SWITCH2__
    (void)flags;
    app_av_playback_callbacks(media->playback)->video_frame(
        media->playback, timestamp_us, flags, data, length);
#else
    (void)flags;
    if (!av_codecs_submit_h264(app_av_playback_codecs(media->playback),
            data, length, timestamp_us))
        dsu_server_av_request_keyframe(media->server, 0, 1, 2);
#endif
}

static bool session_start(void *context, const DsuAvSessionSettings *settings)
{
    SwitchAv *media = context;
    close_media_session(media);
    const DsuAvCallbacks *callbacks =
        app_av_playback_callbacks(media->playback);
    if (!callbacks->session_start(media->playback, settings))
        return false;
    if (settings->microphone_codec != DSU_AV_CODEC_NONE) {
        app_microphone_session_start(&media->microphone);
    }

    media->active = true;
    return true;
}

static void session_stop(void *context)
{
    SwitchAv *media = context;
    close_media_session(media);
    app_av_playback_callbacks(media->playback)->session_stop(
        media->playback);
}

static bool start_microphone(SwitchAv *media)
{
    if (!app_microphone_is_requested(&media->microphone) ||
        media->capture_device)
        return media->capture_device != 0;
    SDL_AudioSpec desired = {
        .freq = app_av_playback_settings(media->playback)->
            microphone_sample_rate_div100 * 100,
        .format = AUDIO_S16SYS,
        .channels = app_av_playback_settings(media->playback)->
            microphone_channels,
        .samples = 480,
    };
    SDL_AudioSpec obtained;
    media->capture_device = SDL_OpenAudioDevice(
        NULL, 1, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!media->capture_device || obtained.freq != desired.freq ||
        obtained.channels != desired.channels) {
        if (media->capture_device)
            SDL_CloseAudioDevice(media->capture_device);
        media->capture_device = 0;
        app_microphone_set_state(&media->microphone,
            APP_MIC_UNAVAILABLE, 3);
        return false;
    }
    SDL_PauseAudioDevice(media->capture_device, 0);
    av_codecs_reset_microphone_encoder(
        app_av_playback_codecs(media->playback));
    app_microphone_set_state(&media->microphone, APP_MIC_CAPTURING, 1);
    return true;
}

static void stop_microphone(SwitchAv *media)
{
    if (media->capture_device) {
        SDL_ClearQueuedAudio(media->capture_device);
        SDL_PauseAudioDevice(media->capture_device, 1);
        SDL_CloseAudioDevice(media->capture_device);
        media->capture_device = 0;
    }
    av_codecs_discard_microphone_input(
        app_av_playback_codecs(media->playback));
    if (app_microphone_is_requested(&media->microphone))
        app_microphone_set_state(&media->microphone, APP_MIC_MUTED, 1);
}

static uint32_t read_microphone_pcm(void *context, int16_t *samples,
    uint32_t capacity)
{
    return SDL_DequeueAudio((SDL_AudioDeviceID)(uintptr_t)context,
        samples, capacity);
}

void switch_av_poll(SwitchAv *media)
{
    if (!media || !media->active)
        return;
    app_av_playback_poll(media->playback);
    if (!media->capture_device ||
        !app_microphone_is_capturing(&media->microphone))
        return;

    if (SDL_GetQueuedAudioSize(media->capture_device) >
        MICROPHONE_QUEUE_LIMIT) {
        SDL_ClearQueuedAudio(media->capture_device);
        av_codecs_discard_microphone_input(
            app_av_playback_codecs(media->playback));
    }
    int encoded = av_codecs_capture_microphone(
        app_av_playback_codecs(media->playback),
        read_microphone_pcm, (void *)(uintptr_t)media->capture_device,
        media->opus_output, sizeof(media->opus_output));
    if (encoded > 0)
        app_microphone_send(&media->microphone,
            av_codecs_monotonic_us(),
            media->opus_output, (uint16_t)encoded);
}

void switch_av_handle_input(SwitchAv *media, uint64_t buttons_down)
{
    if (!media || !app_microphone_is_requested(&media->microphone) ||
        !(buttons_down & HidNpadButton_A))
        return;
    if (app_microphone_is_capturing(&media->microphone))
        stop_microphone(media);
    else
        start_microphone(media);
}

const DsuAvCapabilities *switch_av_capabilities(const SwitchAv *media)
{
    return app_av_playback_capabilities(media->playback);
}

const DsuAvCallbacks *switch_av_callbacks(const SwitchAv *media)
{
    return &media->callbacks;
}

bool switch_av_is_video_visible(const SwitchAv *media)
{
    return media && sdl_display_has_video(media->display);
}

SwitchAv *switch_av_create(DsuServer *server, SdlDisplay *display)
{
    SwitchAv *media = calloc(1, sizeof(*media));
    if (!media)
        return NULL;
    media->server = server;
    app_microphone_init(&media->microphone, server);
    media->display = display;
    media->playback = app_av_playback_create(display, true,
#ifdef __SWITCH2__
        &(AppAvVideoBackend){
            .context = media,
            .video_submit = hardware_video_submit,
        }
#else
        NULL
#endif
    );
    if (!media->playback) {
        free(media);
        return NULL;
    }
    media->callbacks = (DsuAvCallbacks) {
        .session_start = session_start,
        .session_stop = session_stop,
        .video_frame = receive_video,
        .audio_packet =
            app_av_playback_callbacks(media->playback)->audio_packet,
    };
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        app_av_playback_destroy(media->playback);
        free(media);
        return NULL;
    }
    return media;
}

void switch_av_destroy(SwitchAv *media)
{
    if (!media)
        return;
    close_media_session(media);
    app_av_playback_destroy(media->playback);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(media);
}
