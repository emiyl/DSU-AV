#include "av_playback.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

#define MAX_OPUS_SAMPLES 2880
#define AUDIO_RING_BYTES (48000 * 2 * sizeof(int16_t))
#define AUDIO_STATS_INTERVAL_US 1000000ULL
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
    uint32_t audio_packets_decoded;
    uint32_t audio_output_sample_rate;
    double audio_resample_position;
    bool audio_playing;
    uint64_t audio_packets_received;
    uint64_t audio_encoded_bytes;
    uint64_t audio_samples_decoded;
    uint64_t audio_bytes_buffered;
    uint64_t audio_callback_count;
    uint64_t audio_callback_bytes;
    uint64_t audio_underrun_bytes;
    uint64_t next_audio_stats_us;
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
    ++playback->audio_callback_count;
    playback->audio_callback_bytes += (uint64_t)copied;
    if (copied < (uint32_t)length) {
        playback->audio_underrun_bytes +=
            (uint32_t)length - copied;
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
    playback->audio_bytes_buffered += length;
    uint32_t start_bytes = playback->audio_output_sample_rate *
        playback->settings.audio_channels * sizeof(int16_t) / 10;
    bool start = !playback->audio_playing &&
        playback->audio_available >= start_bytes;
    if (start)
        playback->audio_playing = true;
    SDL_UnlockAudioDevice(playback->audio_device);
    if (start) {
        SDL_PauseAudioDevice(playback->audio_device, 0);
        SDL_Log("Audio playback started after buffering %u bytes.",
            (unsigned)start_bytes);
    }
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
        SDL_Log("Audio output opened: %d Hz, %u channels, %u samples",
            obtained.freq, (unsigned)obtained.channels,
            (unsigned)obtained.samples);
        SDL_Log("Audio driver '%s'; %d compiled drivers, device state %d.",
            SDL_GetCurrentAudioDriver() ?
                SDL_GetCurrentAudioDriver() : "none",
            SDL_GetNumAudioDrivers(),
            (int)SDL_GetAudioDeviceStatus(playback->audio_device));
        playback->audio_read = 0;
        playback->audio_write = 0;
        playback->audio_available = 0;
        playback->audio_packets_decoded = 0;
        playback->audio_playing = false;
        playback->audio_output_sample_rate = (uint32_t)obtained.freq;
        playback->audio_resample_position = 0;
        playback->audio_packets_received = 0;
        playback->audio_encoded_bytes = 0;
        playback->audio_samples_decoded = 0;
        playback->audio_bytes_buffered = 0;
        playback->audio_callback_count = 0;
        playback->audio_callback_bytes = 0;
        playback->audio_underrun_bytes = 0;
        playback->next_audio_stats_us =
            av_codecs_monotonic_us() + AUDIO_STATS_INTERVAL_US;
        SDL_PauseAudioDevice(playback->audio_device, 1);
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
    ++playback->audio_packets_received;
    playback->audio_encoded_bytes += length;
    if (playback->audio_packets_received == 1)
        SDL_Log("First DSU audio packet received: sequence %u, %u bytes, "
            "flags 0x%02x.", sequence, (unsigned)length, (unsigned)flags);
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
    playback->audio_samples_decoded += (uint64_t)samples;
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
    ++playback->audio_packets_decoded;
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
    uint64_t now_us = av_codecs_monotonic_us();
    if (now_us >= playback->next_audio_stats_us) {
        uint32_t available = playback->audio_available;
        int device_state = playback->audio_device ?
            (int)SDL_GetAudioDeviceStatus(playback->audio_device) : -1;
        SDL_Log("Audio stats: packets=%llu encoded=%llu decoded_samples=%llu "
            "buffered=%llu callbacks=%llu callback_bytes=%llu "
            "underrun=%llu ring=%u state=%d playing=%d.",
            (unsigned long long)playback->audio_packets_received,
            (unsigned long long)playback->audio_encoded_bytes,
            (unsigned long long)playback->audio_samples_decoded,
            (unsigned long long)playback->audio_bytes_buffered,
            (unsigned long long)playback->audio_callback_count,
            (unsigned long long)playback->audio_callback_bytes,
            (unsigned long long)playback->audio_underrun_bytes,
            (unsigned)available, device_state, playback->audio_playing);
        playback->next_audio_stats_us =
            now_us + AUDIO_STATS_INTERVAL_US;
    }
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
