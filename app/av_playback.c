#include "av_playback.h"

#include <SDL.h>

#include <stdlib.h>

#define MAX_OPUS_SAMPLES 2880
#define AUDIO_QUEUE_LIMIT (48000 * 2 * 2 / 5)

struct AppAvPlayback {
    AvCodecs *codecs;
    DsuAvSessionSettings settings;
    DsuAvCapabilities capabilities;
    DsuAvCallbacks callbacks;
    AppAvVideoBackend video_backend;
    SdlDisplay *display;
    SDL_AudioDeviceID audio_device;
    int16_t pcm[MAX_OPUS_SAMPLES * 2];
    bool active;
};

static bool session_start(void *context,
    const DsuAvSessionSettings *settings)
{
    AppAvPlayback *playback = context;
    if (playback->active)
        return false;
    playback->settings = *settings;
    playback->codecs = av_codecs_create(settings);
    if (!playback->codecs)
        return false;
    if (playback->video_backend.video_start &&
        !playback->video_backend.video_start(
            playback->video_backend.context, settings))
        goto fail;

    if (settings->audio_codec != DSU_AV_CODEC_NONE) {
        SDL_AudioSpec desired = {
            .freq = settings->audio_sample_rate_div100 * 100,
            .format = AUDIO_S16SYS,
            .channels = settings->audio_channels,
            .samples = 256,
        };
        SDL_AudioSpec obtained;
        playback->audio_device = SDL_OpenAudioDevice(
            NULL, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (!playback->audio_device ||
            obtained.freq != desired.freq ||
            obtained.channels != desired.channels)
            goto fail;
        SDL_PauseAudioDevice(playback->audio_device, 0);
    }
    playback->active = true;
    SDL_Log("DSU-AV session started: %ux%u @ %u FPS",
        settings->video_width, settings->video_height,
        settings->video_fps);
    return true;

fail:
    if (playback->video_backend.video_stop)
        playback->video_backend.video_stop(playback->video_backend.context);
    if (playback->audio_device) {
        SDL_CloseAudioDevice(playback->audio_device);
        playback->audio_device = 0;
    }
    av_codecs_destroy(playback->codecs);
    playback->codecs = NULL;
    return false;
}

static void session_stop(void *context)
{
    AppAvPlayback *playback = context;
    sdl_display_clear_video(playback->display);
    if (playback->video_backend.video_stop)
        playback->video_backend.video_stop(playback->video_backend.context);
    if (playback->audio_device) {
        SDL_CloseAudioDevice(playback->audio_device);
        playback->audio_device = 0;
    }
    av_codecs_destroy(playback->codecs);
    playback->codecs = NULL;
    playback->active = false;
}

static void video_frame(void *context, uint64_t timestamp_us, uint8_t flags,
    const uint8_t *data, uint32_t length)
{
    (void)flags;
    AppAvPlayback *playback = context;
    if (!playback->active)
        return;
    if (playback->video_backend.video_submit) {
        playback->video_backend.video_submit(playback->video_backend.context,
            data, length, timestamp_us);
    } else {
        av_codecs_submit_h264(playback->codecs, data, length, timestamp_us);
    }
}

static void audio_packet(void *context, uint32_t sequence,
    uint64_t timestamp_us, uint32_t packet_id, uint8_t flags,
    const uint8_t *data, uint16_t length)
{
    (void)sequence;
    (void)timestamp_us;
    (void)packet_id;
    AppAvPlayback *playback = context;
    if (!playback->audio_device)
        return;
    int samples = av_codecs_decode_opus(playback->codecs, data, length,
        (flags & 0x01) != 0, playback->pcm, MAX_OPUS_SAMPLES);
    if (samples <= 0)
        return;
    if ((uint32_t)SDL_GetQueuedAudioSize(playback->audio_device) >
        AUDIO_QUEUE_LIMIT)
        SDL_ClearQueuedAudio(playback->audio_device);
    SDL_QueueAudio(playback->audio_device, playback->pcm,
        (Uint32)(samples * playback->settings.audio_channels *
            sizeof(int16_t)));
}

static void render_video_frame(void *context, const AvDecodedFrame *frame)
{
    AppAvPlayback *playback = context;
    sdl_display_frame(playback->display, playback->codecs, frame);
}

AppAvPlayback *app_av_playback_create(SdlDisplay *display,
    bool microphone_supported, const AppAvVideoBackend *video_backend)
{
    AppAvPlayback *playback = calloc(1, sizeof(*playback));
    if (!playback)
        return NULL;
    playback->display = display;
    if (video_backend)
        playback->video_backend = *video_backend;
    playback->capabilities = (DsuAvCapabilities){
        .flags = DSU_AV_CAP_VIDEO | DSU_AV_CAP_AUDIO |
            DSU_AV_CAP_KEYFRAME | DSU_AV_CAP_QUALITY |
            (microphone_supported ? DSU_AV_CAP_MICROPHONE : 0),
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
        .microphone_codec = microphone_supported ?
            DSU_AV_CODEC_OPUS : DSU_AV_CODEC_NONE,
        .microphone_sample_rate_div100 = microphone_supported ? 480 : 0,
        .microphone_channels = microphone_supported ? 1 : 0,
        .microphone_frame_duration_01ms = microphone_supported ? 100 : 0,
    };
    playback->callbacks = (DsuAvCallbacks){
        .session_start = session_start,
        .session_stop = session_stop,
        .video_frame = video_frame,
        .audio_packet = audio_packet,
    };
    return playback;
}

void app_av_playback_destroy(AppAvPlayback *playback)
{
    if (!playback)
        return;
    session_stop(playback);
    free(playback);
}

const DsuAvCapabilities *app_av_playback_capabilities(
    const AppAvPlayback *playback)
{
    return &playback->capabilities;
}

const DsuAvCallbacks *app_av_playback_callbacks(
    const AppAvPlayback *playback)
{
    return &playback->callbacks;
}

bool app_av_playback_active(const AppAvPlayback *playback)
{
    return playback && playback->active;
}

void app_av_playback_poll(AppAvPlayback *playback)
{
    if (!playback || !playback->active)
        return;
    if (playback->video_backend.video_poll)
        playback->video_backend.video_poll(playback->video_backend.context,
            render_video_frame, playback);
    else if (!playback->video_backend.video_submit)
        av_codecs_poll_video(playback->codecs,
            render_video_frame, playback);
}

AvCodecs *app_av_playback_codecs(AppAvPlayback *playback)
{
    return playback ? playback->codecs : NULL;
}

const DsuAvSessionSettings *app_av_playback_settings(
    const AppAvPlayback *playback)
{
    return playback && playback->active ? &playback->settings : NULL;
}
