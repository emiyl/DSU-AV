#include <av/av_codecs.h>

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <opus.h>

#define MAX_OPUS_SAMPLES 2880

struct AvCodecs {
#ifdef AV_CODECS_SOFTWARE_H264
    AVCodecContext *h264_context;
    const AVCodec *h264_codec;
    AVPacket *h264_packet;
    AVFrame *decoded_frame;
    AVCodecContext *h264_encoder_context;
    AVPacket *h264_encoder_packet;
    AVFrame *h264_encoder_frame;
#endif
    OpusDecoder *opus_decoder;
    OpusEncoder *opus_audio_encoder;
    OpusEncoder *opus_microphone_encoder;
    struct SwsContext *scaler;
    struct SwsContext *encoder_scaler;
    enum AVPixelFormat scaler_format;
    int scaler_width;
    int scaler_height;
    int scaler_output_width;
    int scaler_output_height;
    int16_t microphone_pcm[MAX_OPUS_SAMPLES * 2];
    uint32_t microphone_pcm_bytes;
    uint32_t microphone_frame_bytes;
    int microphone_frame_samples;
    int microphone_channels;
    int video_width;
    int video_height;
    int video_fps;
    int video_bitrate_kbps;
};

AvCodecs *av_codecs_create(const DsuAvSessionSettings *settings)
{
    AvCodecs *codecs = calloc(1, sizeof(*codecs));
    if (!codecs)
        return NULL;

#ifdef AV_CODECS_SOFTWARE_H264
    if (settings->video_codec == DSU_AV_CODEC_H264) {
        codecs->h264_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        codecs->h264_context = avcodec_alloc_context3(codecs->h264_codec);
        codecs->h264_packet = av_packet_alloc();
        codecs->decoded_frame = av_frame_alloc();
        if (!codecs->h264_codec || !codecs->h264_context ||
            !codecs->h264_packet || !codecs->decoded_frame)
            goto fail;
        codecs->h264_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecs->h264_context->thread_count = 1;
        if (avcodec_open2(codecs->h264_context, codecs->h264_codec, NULL) < 0)
            goto fail;
    }
#endif

    if (settings->audio_codec == DSU_AV_CODEC_OPUS) {
        int error = OPUS_OK;
        codecs->opus_decoder = opus_decoder_create(
            settings->audio_sample_rate_div100 * 100,
            settings->audio_channels, &error);
        if (error != OPUS_OK)
            goto fail;
        codecs->opus_audio_encoder = opus_encoder_create(
            settings->audio_sample_rate_div100 * 100,
            settings->audio_channels, OPUS_APPLICATION_AUDIO, &error);
        if (error != OPUS_OK)
            goto fail;
    }

    if (settings->microphone_codec == DSU_AV_CODEC_OPUS) {
        int error = OPUS_OK;
        codecs->opus_microphone_encoder = opus_encoder_create(
            settings->microphone_sample_rate_div100 * 100,
            settings->microphone_channels, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK)
            goto fail;
        opus_encoder_ctl(codecs->opus_microphone_encoder,
            OPUS_SET_BITRATE(24000));
        codecs->microphone_frame_samples =
            settings->microphone_frame_duration_01ms *
            (settings->microphone_sample_rate_div100 * 100) / 10000;
        codecs->microphone_channels = settings->microphone_channels;
        codecs->microphone_frame_bytes =
            (uint32_t)codecs->microphone_frame_samples *
            codecs->microphone_channels * sizeof(int16_t);
    }
    codecs->video_width = settings->video_width;
    codecs->video_height = settings->video_height;
    codecs->video_fps = settings->video_fps;
    codecs->video_bitrate_kbps = settings->video_bitrate_kbps;
    return codecs;

fail:
    av_codecs_destroy(codecs);
    return NULL;
}

void av_codecs_destroy(AvCodecs *codecs)
{
    if (!codecs)
        return;
    if (codecs->scaler)
        sws_freeContext(codecs->scaler);
    if (codecs->encoder_scaler)
        sws_freeContext(codecs->encoder_scaler);
    if (codecs->opus_decoder)
        opus_decoder_destroy(codecs->opus_decoder);
    if (codecs->opus_audio_encoder)
        opus_encoder_destroy(codecs->opus_audio_encoder);
    if (codecs->opus_microphone_encoder)
        opus_encoder_destroy(codecs->opus_microphone_encoder);
#ifdef AV_CODECS_SOFTWARE_H264
    av_packet_free(&codecs->h264_encoder_packet);
    av_frame_free(&codecs->h264_encoder_frame);
    avcodec_free_context(&codecs->h264_encoder_context);
    av_packet_free(&codecs->h264_packet);
    av_frame_free(&codecs->decoded_frame);
    avcodec_free_context(&codecs->h264_context);
#endif
    free(codecs);
}

bool av_codecs_submit_h264(AvCodecs *codecs, const uint8_t *data,
    uint32_t length, uint64_t timestamp_us)
{
#ifndef AV_CODECS_SOFTWARE_H264
    (void)codecs;
    (void)data;
    (void)length;
    (void)timestamp_us;
    return false;
#else
    if (!codecs->h264_context || !codecs->h264_packet || length > INT32_MAX)
        return false;
    av_packet_unref(codecs->h264_packet);
    codecs->h264_packet->data = (uint8_t *)data;
    codecs->h264_packet->size = (int)length;
    codecs->h264_packet->pts = (int64_t)timestamp_us;
    bool accepted =
        avcodec_send_packet(codecs->h264_context, codecs->h264_packet) >= 0;
    codecs->h264_packet->data = NULL;
    codecs->h264_packet->size = 0;
    return accepted;
#endif
}

void av_codecs_poll_video(AvCodecs *codecs, AvDecodedFrameCallback callback,
    void *context)
{
#ifdef AV_CODECS_SOFTWARE_H264
    if (!codecs || !codecs->h264_context || !codecs->decoded_frame ||
        !callback)
        return;
    while (avcodec_receive_frame(codecs->h264_context,
            codecs->decoded_frame) == 0) {
        AvDecodedFrame frame = {
            .timestamp_us =
                (uint64_t)codecs->decoded_frame->best_effort_timestamp,
            .planes = (const uint8_t *const *)codecs->decoded_frame->data,
            .strides = codecs->decoded_frame->linesize,
            .format = (enum AVPixelFormat)codecs->decoded_frame->format,
            .width = codecs->decoded_frame->width,
            .height = codecs->decoded_frame->height,
        };
        callback(context, &frame);
        av_frame_unref(codecs->decoded_frame);
    }
#else
    (void)codecs;
    (void)callback;
    (void)context;
#endif
}

bool av_codecs_extract_h264_config(const uint8_t *data, uint32_t length,
    uint8_t *configuration, uint32_t configuration_capacity,
    uint32_t *configuration_length)
{
    bool found_sps = false;
    bool found_pps = false;
    uint32_t output_length = 0;

    for (uint32_t i = 0; i + 4 < length;) {
        uint32_t start_size = 0;
        if (data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1)
            start_size = 4;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            start_size = 3;
        else {
            ++i;
            continue;
        }

        uint32_t nal_end = i + start_size + 1;
        while (nal_end + 2 < length &&
            !(data[nal_end] == 0 && data[nal_end + 1] == 0 &&
                (data[nal_end + 2] == 1 ||
                    (nal_end + 3 < length && data[nal_end + 2] == 0 &&
                        data[nal_end + 3] == 1))))
            ++nal_end;
        uint8_t nal_type = data[i + start_size] & 0x1f;
        uint32_t nal_size = nal_end - i;
        if (nal_type == 7)
            found_sps = true;
        else if (nal_type == 8)
            found_pps = true;
        else {
            i = nal_end;
            continue;
        }
        if (output_length + nal_size > configuration_capacity)
            return false;
        memcpy(configuration + output_length, data + i, nal_size);
        output_length += nal_size;
        i = nal_end;
    }

    *configuration_length = output_length;
    return found_sps && found_pps;
}

#ifdef AV_CODECS_SOFTWARE_H264
static bool av_codecs_init_h264_encoder(AvCodecs *codecs)
{
    if (codecs->h264_encoder_context)
        return true;
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!context || !packet || !frame)
        goto fail;
    context->width = codecs->video_width;
    context->height = codecs->video_height;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = (AVRational){1, codecs->video_fps};
    context->framerate = (AVRational){codecs->video_fps, 1};
    context->bit_rate = (int64_t)codecs->video_bitrate_kbps * 1000;
    context->gop_size = codecs->video_fps * 2;
    context->max_b_frames = 0;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_count = 1;
    av_opt_set(context->priv_data, "preset", "ultrafast", 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(context, codec, NULL) < 0)
        goto fail;

    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    if (av_frame_get_buffer(frame, 32) < 0)
        goto fail;

    codecs->h264_encoder_context = context;
    codecs->h264_encoder_packet = packet;
    codecs->h264_encoder_frame = frame;
    return true;

fail:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return false;
}
#endif

int av_codecs_encode_rgba(AvCodecs *codecs, const uint8_t *rgba, int stride,
    uint64_t timestamp_us, bool force_keyframe, bool *is_keyframe,
    uint8_t *data, int capacity)
{
#ifndef AV_CODECS_SOFTWARE_H264
    (void)codecs;
    (void)rgba;
    (void)stride;
    (void)timestamp_us;
    (void)force_keyframe;
    (void)is_keyframe;
    (void)data;
    (void)capacity;
    return 0;
#else
    if (!codecs || !rgba || !data || capacity <= 0 || stride <= 0 ||
        !av_codecs_init_h264_encoder(codecs))
        return 0;
    AVFrame *frame = codecs->h264_encoder_frame;
    if (av_frame_make_writable(frame) < 0)
        return 0;

    if (!codecs->encoder_scaler) {
        codecs->encoder_scaler = sws_getContext(frame->width, frame->height,
            AV_PIX_FMT_RGBA, frame->width, frame->height,
            (enum AVPixelFormat)frame->format, SWS_BILINEAR,
            NULL, NULL, NULL);
    }
    if (!codecs->encoder_scaler)
        return 0;
    const uint8_t *source[] = {rgba, NULL, NULL, NULL};
    int source_stride[] = {stride, 0, 0, 0};
    if (sws_scale(codecs->encoder_scaler, source, source_stride, 0,
            frame->height, frame->data, frame->linesize) != frame->height)
        return 0;

    frame->pts = (int64_t)(timestamp_us * (uint64_t)codecs->video_fps /
        1000000ULL);
#if LIBAVUTIL_VERSION_MAJOR >= 58
    if (force_keyframe)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;
#else
    frame->key_frame = force_keyframe ? 1 : 0;
    frame->pict_type = force_keyframe ? AV_PICTURE_TYPE_I
        : AV_PICTURE_TYPE_NONE;
#endif
    if (avcodec_send_frame(codecs->h264_encoder_context, frame) < 0)
        return 0;
    AVPacket *packet = codecs->h264_encoder_packet;
    av_packet_unref(packet);
    if (avcodec_receive_packet(codecs->h264_encoder_context, packet) < 0)
        return 0;
    if (packet->size > capacity)
        return 0;
    memcpy(data, packet->data, (size_t)packet->size);
    if (is_keyframe)
        *is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    return packet->size;
#endif
}

int av_codecs_decode_opus(AvCodecs *codecs, const uint8_t *data,
    uint16_t length, bool reset, int16_t *pcm, int pcm_capacity)
{
    if (!codecs || !codecs->opus_decoder)
        return 0;
    if (reset)
        opus_decoder_ctl(codecs->opus_decoder, OPUS_RESET_STATE);
    int decoded = opus_decode(codecs->opus_decoder, data, length, pcm,
        pcm_capacity, 0);
    return decoded > 0 ? decoded : 0;
}

int av_codecs_encode_opus(AvCodecs *codecs, const int16_t *pcm,
    int sample_count, uint8_t *data, int capacity)
{
    if (!codecs || !codecs->opus_audio_encoder || capacity <= 0)
        return 0;
    int encoded = opus_encode(codecs->opus_audio_encoder, pcm, sample_count,
        data, capacity);
    return encoded > 0 ? encoded : 0;
}

int av_codecs_capture_microphone(AvCodecs *codecs,
    AvMicrophoneReadCallback read_callback, void *read_context,
    uint8_t *data, int capacity)
{
    if (!codecs || !codecs->opus_microphone_encoder || !read_callback ||
        codecs->microphone_frame_bytes == 0)
        return 0;
    while (codecs->microphone_pcm_bytes < codecs->microphone_frame_bytes) {
        uint32_t missing = codecs->microphone_frame_bytes -
            codecs->microphone_pcm_bytes;
        uint32_t received = read_callback(read_context,
            (int16_t *)((uint8_t *)codecs->microphone_pcm +
                codecs->microphone_pcm_bytes), missing);
        if (received == 0)
            return 0;
        codecs->microphone_pcm_bytes += received > missing
            ? missing : received;
    }
    int encoded = opus_encode(codecs->opus_microphone_encoder,
        codecs->microphone_pcm,
        codecs->microphone_frame_samples, data, capacity);
    codecs->microphone_pcm_bytes = 0;
    return encoded > 0 ? encoded : 0;
}

void av_codecs_discard_microphone_input(AvCodecs *codecs)
{
    if (codecs)
        codecs->microphone_pcm_bytes = 0;
}

void av_codecs_reset_microphone_encoder(AvCodecs *codecs)
{
    if (!codecs || !codecs->opus_microphone_encoder)
        return;
    opus_encoder_ctl(codecs->opus_microphone_encoder, OPUS_RESET_STATE);
    codecs->microphone_pcm_bytes = 0;
}

uint64_t av_codecs_monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
        (uint64_t)now.tv_nsec / 1000ULL;
}

bool av_codecs_scale_to_rgba(AvCodecs *codecs, const AvDecodedFrame *frame,
    uint8_t *destination, int destination_stride, int destination_width,
    int destination_height)
{
    if (!codecs || !frame || !destination || frame->width <= 0 ||
        frame->height <= 0)
        return false;
    if (!codecs->scaler || codecs->scaler_format != frame->format ||
        codecs->scaler_width != frame->width ||
        codecs->scaler_height != frame->height ||
        codecs->scaler_output_width != destination_width ||
        codecs->scaler_output_height != destination_height) {
        sws_freeContext(codecs->scaler);
        codecs->scaler = sws_getContext(frame->width, frame->height,
            frame->format, destination_width, destination_height,
            AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
        codecs->scaler_format = frame->format;
        codecs->scaler_width = frame->width;
        codecs->scaler_height = frame->height;
        codecs->scaler_output_width = destination_width;
        codecs->scaler_output_height = destination_height;
    }
    if (!codecs->scaler)
        return false;
    uint8_t *destination_planes[4] = {destination, NULL, NULL, NULL};
    int destination_strides[4] = {destination_stride, 0, 0, 0};
    return sws_scale(codecs->scaler, frame->planes, frame->strides, 0,
        frame->height, destination_planes, destination_strides) > 0;
}
