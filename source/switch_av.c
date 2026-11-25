#include "switch_av.h"
#include <av/av_codecs.h>

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
#define AUDIO_QUEUE_LIMIT (48000 * 2 * 2 / 5)
#define MICROPHONE_QUEUE_LIMIT (48000 * 2 / 5)

struct SwitchAv {
    DsuServer *server;
    DsuAvCapabilities capabilities;
    DsuAvCallbacks callbacks;
    DsuAvSessionSettings settings;
    bool active;
    bool framebuffer_active;
    bool console_visible;
    bool microphone_enabled;
    bool microphone_requested;
    uint8_t microphone_state;

    SDL_AudioDeviceID output_device;
    SDL_AudioDeviceID capture_device;
    AvCodecs *codecs;
    uint8_t opus_output[1500];
    int16_t opus_pcm[MAX_OPUS_SAMPLES * 2];

    Framebuffer framebuffer;

#ifdef __SWITCH2__
    VideoDecoder decoder;
    bool decoder_created;
    uint8_t codec_config[8192];
    uint32_t codec_config_size;
#endif
};

static void restore_console(SwitchAv *media)
{
    if (media->framebuffer_active) {
        framebufferClose(&media->framebuffer);
        media->framebuffer_active = false;
    }
    if (!media->console_visible) {
        consoleInit(NULL);
        media->console_visible = true;
    }
}

static bool prepare_framebuffer(SwitchAv *media)
{
    if (media->framebuffer_active)
        return true;
    if (media->console_visible) {
        consoleExit(NULL);
        media->console_visible = false;
    }
    Result rc = framebufferCreate(&media->framebuffer, nwindowGetDefault(),
        DISPLAY_WIDTH, DISPLAY_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc))
        goto fail;
    if (R_FAILED(framebufferMakeLinear(&media->framebuffer))) {
        framebufferClose(&media->framebuffer);
        goto fail;
    }
    media->framebuffer_active = true;
    return true;

fail:
    consoleInit(NULL);
    media->console_visible = true;
    return false;
}

static void render_frame(SwitchAv *media, const AvDecodedFrame *frame)
{
    if (!media->framebuffer_active || !frame)
        return;

    uint32_t stride = 0;
    uint8_t *destination = framebufferBegin(&media->framebuffer, &stride);
    if (!destination)
        return;
    if (!av_codecs_scale_to_rgba(media->codecs, frame, destination,
            (int)stride, DISPLAY_WIDTH, DISPLAY_HEIGHT))
        goto present;
    if (media->microphone_enabled) {
        const uint32_t indicator = RGBA8(255, 48, 48, 255);
        for (int y = 8; y < 20; ++y) {
            uint32_t *row =
                (uint32_t *)(destination + (size_t)y * stride);
            for (int x = DISPLAY_WIDTH - 20; x < DISPLAY_WIDTH - 8; ++x)
                row[x] = indicator;
        }
    }
present:
    framebufferEnd(&media->framebuffer);
}

#ifndef __SWITCH2__
static void decoded_video_frame(void *context, const AvDecodedFrame *frame)
{
    render_frame(context, frame);
}
#endif

#ifdef __SWITCH2__
static void decode_hardware_video(SwitchAv *media, const uint8_t *data,
    uint32_t length, uint64_t timestamp_us)
{
    if (!media->decoder_created) {
        if (!av_codecs_extract_h264_config(data, length, media->codec_config,
                sizeof(media->codec_config), &media->codec_config_size))
            return;
        VideoDecoderConfig config = {
            .codec = ViddecCodec_H264,
            .width = media->settings.video_width,
            .height = media->settings.video_height,
            .frame_rate = media->settings.video_fps,
            .tiling = ViddecTilingMode_PitchLinear,
            .max_sample_size = MAX_ENCODED_FRAME,
            .codec_config = media->codec_config,
            .codec_config_size = media->codec_config_size,
        };
        if (R_FAILED(videoDecoderCreate(&media->decoder, &config)))
            return;
        media->decoder_created = true;
    }

    if (R_FAILED(videoDecoderSubmit(&media->decoder, data, length,
            (s64)timestamp_us * 1000, false, 0))) {
        dsu_server_av_request_keyframe(media->server, 0, 1, 2);
        return;
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
        render_frame(media, &decoded);
        videoDecoderReleaseFrame(&media->decoder, &frame);
    }
}
#endif

static void video_frame(void *context, uint64_t timestamp_us, uint8_t flags,
    const uint8_t *data, uint32_t length)
{
    SwitchAv *media = context;
    (void)flags;
    if (!media->active || length > MAX_ENCODED_FRAME)
        return;
#ifdef __SWITCH2__
    decode_hardware_video(media, data, length, timestamp_us);
#else
    av_codecs_submit_h264(media->codecs, data, length, timestamp_us);
#endif
}

static void audio_packet(void *context, uint32_t sequence,
    uint64_t timestamp_us, uint32_t packet_id, uint8_t flags,
    const uint8_t *data, uint16_t length)
{
    (void)sequence;
    (void)timestamp_us;
    (void)packet_id;
    SwitchAv *media = context;
    if (!media->output_device)
        return;
    int samples = av_codecs_decode_opus(media->codecs, data, length,
        (flags & 0x01) != 0, media->opus_pcm, MAX_OPUS_SAMPLES);
    if (samples <= 0)
        return;
    if ((uint32_t)SDL_GetQueuedAudioSize(media->output_device) >
        AUDIO_QUEUE_LIMIT)
        SDL_ClearQueuedAudio(media->output_device);
    SDL_QueueAudio(media->output_device, media->opus_pcm,
        (Uint32)(samples * media->settings.audio_channels * sizeof(int16_t)));
}

static void close_media_session(SwitchAv *media)
{
    if (media->capture_device) {
        SDL_PauseAudioDevice(media->capture_device, 1);
        SDL_CloseAudioDevice(media->capture_device);
        media->capture_device = 0;
    }
    if (media->output_device) {
        SDL_CloseAudioDevice(media->output_device);
        media->output_device = 0;
    }
    av_codecs_destroy(media->codecs);
    media->codecs = NULL;
#ifdef __SWITCH2__
    if (media->decoder_created) {
        videoDecoderClose(&media->decoder);
        media->decoder_created = false;
    }
#endif
    media->active = false;
    media->microphone_enabled = false;
    media->microphone_requested = false;
    media->microphone_state = 0;
    restore_console(media);
}

static bool session_start(void *context, const DsuAvSessionSettings *settings)
{
    SwitchAv *media = context;
    close_media_session(media);
    media->settings = *settings;
    media->codecs = av_codecs_create(settings);
    if (!media->codecs)
        goto fail;

    if (settings->video_codec != DSU_AV_CODEC_NONE) {
        if (!prepare_framebuffer(media))
            goto fail;
    }

    if (settings->audio_codec != DSU_AV_CODEC_NONE) {
        SDL_AudioSpec desired = {
            .freq = settings->audio_sample_rate_div100 * 100,
            .format = AUDIO_S16SYS,
            .channels = settings->audio_channels,
            .samples = 256,
        };
        SDL_AudioSpec obtained;
        media->output_device = SDL_OpenAudioDevice(
            NULL, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (!media->output_device ||
            obtained.freq != desired.freq ||
            obtained.channels != desired.channels) {
            goto fail;
        }
        SDL_PauseAudioDevice(media->output_device, 0);
    }

    if (settings->microphone_codec != DSU_AV_CODEC_NONE) {
        media->microphone_requested = true;
        media->microphone_state = 1;
    }

    media->active = true;
    return true;

fail:
    close_media_session(media);
    return false;
}

static void session_stop(void *context)
{
    close_media_session(context);
}

static bool start_microphone(SwitchAv *media)
{
    if (!media->microphone_requested || media->capture_device)
        return media->capture_device != 0;
    SDL_AudioSpec desired = {
        .freq = media->settings.microphone_sample_rate_div100 * 100,
        .format = AUDIO_S16SYS,
        .channels = media->settings.microphone_channels,
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
        media->microphone_state = 4;
        dsu_server_av_set_microphone_state(media->server, 4, 3);
        return false;
    }
    SDL_PauseAudioDevice(media->capture_device, 0);
    av_codecs_reset_microphone_encoder(media->codecs);
    media->microphone_enabled = true;
    media->microphone_state = 3;
    dsu_server_av_set_microphone_state(media->server, 3, 1);
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
    media->microphone_enabled = false;
    av_codecs_discard_microphone_input(media->codecs);
    if (media->microphone_requested)
        media->microphone_state = 2;
    if (media->microphone_requested)
        dsu_server_av_set_microphone_state(media->server, 2, 1);
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
#ifndef __SWITCH2__
    av_codecs_poll_video(media->codecs, decoded_video_frame, media);
#endif
    if (!media->capture_device || !media->microphone_enabled)
        return;

    if (SDL_GetQueuedAudioSize(media->capture_device) >
        MICROPHONE_QUEUE_LIMIT) {
        SDL_ClearQueuedAudio(media->capture_device);
        av_codecs_discard_microphone_input(media->codecs);
    }
    int encoded = av_codecs_capture_microphone(media->codecs,
        read_microphone_pcm, (void *)(uintptr_t)media->capture_device,
        media->opus_output, sizeof(media->opus_output));
    if (encoded > 0)
        dsu_server_av_send_microphone(media->server,
            av_codecs_monotonic_us(),
            0, media->opus_output, (uint16_t)encoded);
}

void switch_av_handle_input(SwitchAv *media, uint64_t buttons_down)
{
    if (!media || !media->microphone_requested ||
        !(buttons_down & HidNpadButton_A))
        return;
    if (media->microphone_enabled)
        stop_microphone(media);
    else
        start_microphone(media);
}

const DsuAvCapabilities *switch_av_capabilities(const SwitchAv *media)
{
    return &media->capabilities;
}

const DsuAvCallbacks *switch_av_callbacks(const SwitchAv *media)
{
    return &media->callbacks;
}

bool switch_av_is_video_visible(const SwitchAv *media)
{
    return media && media->framebuffer_active;
}

SwitchAv *switch_av_create(DsuServer *server)
{
    SwitchAv *media = calloc(1, sizeof(*media));
    if (!media)
        return NULL;
    media->server = server;
    media->console_visible = true;
    media->capabilities = (DsuAvCapabilities) {
        .flags = DSU_AV_CAP_VIDEO | DSU_AV_CAP_AUDIO |
            DSU_AV_CAP_KEYFRAME | DSU_AV_CAP_QUALITY |
            DSU_AV_CAP_MICROPHONE | DSU_AV_CAP_UNAUTHENTICATED,
        .video_codec = DSU_AV_CODEC_H264,
        .audio_codec = DSU_AV_CODEC_OPUS,
        .video_width = DISPLAY_WIDTH,
        .video_height = DISPLAY_HEIGHT,
        .video_fps = 60,
        .video_bitrate_kbps = 8000,
        .audio_sample_rate_div100 = 480,
        .audio_channels = 2,
        .audio_frame_duration_01ms = 100,
        .payload_limit = 1100,
        .microphone_codec = DSU_AV_CODEC_OPUS,
        .microphone_sample_rate_div100 = 480,
        .microphone_channels = 1,
        .microphone_frame_duration_01ms = 100,
    };
    media->callbacks = (DsuAvCallbacks) {
        .session_start = session_start,
        .session_stop = session_stop,
        .video_frame = video_frame,
        .audio_packet = audio_packet,
    };
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
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
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(media);
}
