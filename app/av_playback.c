#include "av_playback.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

#define MAX_OPUS_SAMPLES 2880
#define AUDIO_RING_BYTES (48000 * 2 * sizeof(int16_t))
#define DSU_VIDEO_WIDTH 640
#define DSU_VIDEO_HEIGHT 360
#define DSU_VIDEO_FPS 30
#define DSU_VIDEO_BITRATE_KBPS 3000
#ifdef __SWITCH__
#define AUDIO_OUTPUT_SAMPLE_RATE 40960
#else
#define AUDIO_OUTPUT_SAMPLE_RATE 48000
#endif

struct AppAvPlayback {
    AvCodecs *codecs;
    DsuAvSessionSettings settings;
    DsuAvCapabilities capabilities;
    DsuAvCallbacks callbacks;
    AppAvVideoBackend video_backend;
    SdlDisplay *display;
    SDL_AudioDeviceID audio_device;
    int16_t pcm[MAX_OPUS_SAMPLES * 2];
    uint8_t audio_ring[AUDIO_RING_BYTES];
    uint32_t audio_read;
    uint32_t audio_write;
    uint32_t audio_available;
    uint32_t audio_output_sample_rate;
    double audio_resample_position;
    bool audio_playing;
    bool active;
};

static void render_audio(void *context, uint8_t *output, int length)
{
    AppAvPlayback *playback = context;
    uint32_t copied = (uint32_t)length;
    if (copied > playback->audio_available)
        copied = playback->audio_available;
    for (uint32_t i = 0; i < copied; ++i) {
        output[i] = playback->audio_ring[playback->audio_read];
        playback->audio_read =
            (playback->audio_read + 1) % AUDIO_RING_BYTES;
    }
    playback->audio_available -= copied;
    if (copied < (uint32_t)length) {
        memset(output + copied, 0, (size_t)length - copied);
    }
}

static void queue_audio(AppAvPlayback *playback, const uint8_t *data,
    uint32_t length)
{
    if (length > AUDIO_RING_BYTES)
        length = AUDIO_RING_BYTES;
    SDL_LockAudioDevice(playback->audio_device);
    if (length > AUDIO_RING_BYTES - playback->audio_available) {
        uint32_t discard =
            length - (AUDIO_RING_BYTES - playback->audio_available);
        playback->audio_read = (playback->audio_read + discard) %
            AUDIO_RING_BYTES;
        playback->audio_available -= discard;
    }
    for (uint32_t i = 0; i < length; ++i) {
        playback->audio_ring[playback->audio_write] = data[i];
        playback->audio_write =
            (playback->audio_write + 1) % AUDIO_RING_BYTES;
    }
    playback->audio_available += length;
    uint32_t start_bytes = playback->audio_output_sample_rate *
        playback->settings.audio_channels * sizeof(int16_t) / 10;
    bool start = !playback->audio_playing &&
        playback->audio_available >= start_bytes;
    if (start)
        playback->audio_playing = true;
    SDL_UnlockAudioDevice(playback->audio_device);
    if (start)
        SDL_PauseAudioDevice(playback->audio_device, 0);
}

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

    if (settings->audio_codec != DSU_AV_CODEC_NONE &&
        playback->video_backend.audio_start &&
        !playback->video_backend.audio_start(
            playback->video_backend.context, settings)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
            "Optional audio backend failed; continuing without sound.");
    }

    if (settings->audio_codec != DSU_AV_CODEC_NONE &&
        !playback->video_backend.audio_start) {
        SDL_AudioSpec desired = {
            .freq = AUDIO_OUTPUT_SAMPLE_RATE,
            .format = AUDIO_S16SYS,
            .channels = settings->audio_channels,
#ifdef __SWITCH__
            .samples = 480,
#else
            .samples = 256,
#endif
            .callback = render_audio,
            .userdata = playback,
        };
        SDL_AudioSpec obtained;
        playback->audio_device = SDL_OpenAudioDevice(
            NULL, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
        if (!playback->audio_device ||
            obtained.freq != desired.freq ||
            obtained.channels != desired.channels) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                "Could not open requested audio device (%d Hz, %u channels, "
                "format 0x%x): %s", desired.freq,
                (unsigned)desired.channels, (unsigned)desired.format,
                SDL_GetError());
            goto fail;
        }
        playback->audio_read = 0;
        playback->audio_write = 0;
        playback->audio_available = 0;
        playback->audio_playing = false;
        playback->audio_output_sample_rate = (uint32_t)obtained.freq;
        playback->audio_resample_position = 0;
        SDL_PauseAudioDevice(playback->audio_device, 1);
    }
    playback->active = true;
    return true;

fail:
    if (playback->video_backend.video_stop)
        playback->video_backend.video_stop(playback->video_backend.context);
    if (playback->audio_device) {
        SDL_PauseAudioDevice(playback->audio_device, 1);
        SDL_CloseAudioDevice(playback->audio_device);
        playback->audio_device = 0;
    }
    if (playback->video_backend.audio_stop)
        playback->video_backend.audio_stop(playback->video_backend.context);
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
        playback->video_backend.video_submit(
            playback->video_backend.context, data, length, timestamp_us);
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
    if (!playback->audio_device &&
        !playback->video_backend.audio_submit) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
            "Received audio before an output device was available.");
        return;
    }
    int samples = av_codecs_decode_opus(playback->codecs, data, length,
        (flags & 0x01) != 0, playback->pcm, MAX_OPUS_SAMPLES);
    if (samples <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
            "Could not decode DSU Opus audio packet (%u bytes).",
            (unsigned)length);
        return;
    }
    int output_samples = samples;
    int output_pcm_count = samples;
    if (playback->audio_output_sample_rate &&
        playback->audio_output_sample_rate !=
            playback->settings.audio_sample_rate_div100 * 100) {
        int input_rate =
            playback->settings.audio_sample_rate_div100 * 100;
        double ratio = (double)input_rate /
            playback->audio_output_sample_rate;
        output_samples = (int)((double)samples / ratio);
        for (int sample = 0; sample < output_samples; ++sample) {
            int source = (int)playback->audio_resample_position;
            if (source >= samples)
                source = samples - 1;
            for (int channel = 0;
                channel < playback->settings.audio_channels; ++channel) {
                playback->pcm[sample *
                    playback->settings.audio_channels + channel] =
                    ((const int16_t *)playback->pcm)[source *
                        playback->settings.audio_channels + channel];
            }
            playback->audio_resample_position += ratio;
        }
        playback->audio_resample_position -= samples;
        if (playback->audio_resample_position < 0)
            playback->audio_resample_position = 0;
        output_pcm_count = output_samples;
    }
    if (playback->video_backend.audio_submit) {
        if (!playback->video_backend.audio_submit(
                playback->video_backend.context, playback->pcm,
                (uint32_t)output_pcm_count,
                playback->settings.audio_channels)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                "Switch audio backend could not submit decoded PCM.");
        }
    } else {
        queue_audio(playback, (const uint8_t *)playback->pcm,
            (uint32_t)(output_pcm_count *
                playback->settings.audio_channels *
                sizeof(int16_t)));
    }
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
        .video_width = DSU_VIDEO_WIDTH,
        .video_height = DSU_VIDEO_HEIGHT,
        .video_fps = DSU_VIDEO_FPS,
        .video_bitrate_kbps = DSU_VIDEO_BITRATE_KBPS,
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
