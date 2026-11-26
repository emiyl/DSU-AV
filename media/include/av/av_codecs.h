#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <libavutil/pixfmt.h>
#include <dsu/dsu_server.h>

typedef struct AvCodecs AvCodecs;

typedef struct {
    uint64_t timestamp_us;
    const uint8_t *const *planes;
    const int *strides;
    enum AVPixelFormat format;
    int width;
    int height;
} AvDecodedFrame;

typedef void (*AvDecodedFrameCallback)(void *context,
    const AvDecodedFrame *frame);
typedef uint32_t (*AvMicrophoneReadCallback)(void *context,
    int16_t *samples, uint32_t capacity);

AvCodecs *av_codecs_create(const DsuAvSessionSettings *settings);
void av_codecs_destroy(AvCodecs *codecs);

bool av_codecs_submit_h264(AvCodecs *codecs, const uint8_t *data,
    uint32_t length, uint64_t timestamp_us);
void av_codecs_poll_video(AvCodecs *codecs, AvDecodedFrameCallback callback,
    void *context);

bool av_codecs_extract_h264_config(const uint8_t *data, uint32_t length,
    uint8_t *configuration, uint32_t configuration_capacity,
    uint32_t *configuration_length);
int av_codecs_encode_rgba(AvCodecs *codecs, const uint8_t *rgba,
    int stride, uint64_t timestamp_us, bool force_keyframe,
    bool *is_keyframe, uint8_t *data, int capacity);
int av_codecs_encode_rgba_scaled(AvCodecs *codecs, const uint8_t *rgb,
    int source_width, int source_height, int stride, uint64_t timestamp_us,
    bool force_keyframe, bool *is_keyframe, uint8_t *data, int capacity);

int av_codecs_decode_opus(AvCodecs *codecs, const uint8_t *data,
    uint16_t length, bool reset, int16_t *pcm, int pcm_capacity);
int av_codecs_encode_opus(AvCodecs *codecs, const int16_t *pcm,
    int sample_count, uint8_t *data, int capacity);
int av_codecs_capture_microphone(AvCodecs *codecs,
    AvMicrophoneReadCallback read_callback, void *read_context,
    uint8_t *data, int capacity);
void av_codecs_discard_microphone_input(AvCodecs *codecs);
void av_codecs_reset_microphone_encoder(AvCodecs *codecs);

uint64_t av_codecs_monotonic_us(void);

bool av_codecs_scale_to_rgba(AvCodecs *codecs,
    const AvDecodedFrame *frame, uint8_t *destination, int destination_stride,
    int destination_width, int destination_height);
