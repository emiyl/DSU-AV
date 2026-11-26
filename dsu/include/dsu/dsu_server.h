#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DSU_PORT 26760

#define DSU_AV_VERSION 1
#define DSU_AV_MSG_CAPABILITIES 0x200000U
#define DSU_AV_MSG_SESSION_START 0x200001U
#define DSU_AV_MSG_SESSION_STOP 0x200002U
#define DSU_AV_MSG_KEYFRAME_REQUEST 0x200003U
#define DSU_AV_MSG_QUALITY_FEEDBACK 0x200004U
#define DSU_AV_MSG_VIDEO 0x200010U
#define DSU_AV_MSG_AUDIO 0x200011U
#define DSU_AV_MSG_MICROPHONE 0x200012U
#define DSU_AV_MSG_MICROPHONE_STATE 0x200013U

#define DSU_AV_CODEC_NONE 0
#define DSU_AV_CODEC_H264 1
#define DSU_AV_CODEC_OPUS 2

#define DSU_AV_CAP_VIDEO (1U << 0)
#define DSU_AV_CAP_AUDIO (1U << 1)
#define DSU_AV_CAP_KEYFRAME (1U << 2)
#define DSU_AV_CAP_QUALITY (1U << 3)
#define DSU_AV_CAP_MICROPHONE (1U << 4)
#define DSU_AV_CAP_UNAUTHENTICATED (1U << 31)

typedef struct {
    bool active;
    uint8_t id;
    uint16_t x;
    uint16_t y;
} DsuTouchPoint;

typedef struct {
    bool connected;
    uint8_t button_masks[2];
    uint8_t home_button;
    uint8_t touch_button;
    DsuTouchPoint touch[2];
    uint8_t left_stick_x;
    uint8_t left_stick_y;
    uint8_t right_stick_x;
    uint8_t right_stick_y;
    uint8_t analog_buttons[16];
    float acceleration[3];
    float angular_velocity[3];
    uint64_t motion_timestamp_us;
    bool motion_valid;
} DsuControllerState;

typedef struct {
    uint32_t server_id;
    uint8_t controller_mac[6];
} DsuServerConfig;

typedef struct {
    uint8_t video_codec;
    uint8_t audio_codec;
    uint16_t video_width;
    uint16_t video_height;
    uint16_t video_fps;
    uint16_t video_bitrate_kbps;
    uint16_t audio_sample_rate_div100;
    uint8_t audio_channels;
    uint8_t audio_frame_duration_01ms;
    uint16_t payload_limit;
    uint8_t microphone_codec;
    uint16_t microphone_sample_rate_div100;
    uint8_t microphone_channels;
    uint8_t microphone_frame_duration_01ms;
    uint8_t microphone_consent_mode;
} DsuAvSessionSettings;

typedef struct {
    uint32_t flags;
    uint8_t video_codec;
    uint8_t audio_codec;
    uint16_t video_width;
    uint16_t video_height;
    uint16_t video_fps;
    uint16_t video_bitrate_kbps;
    uint16_t audio_sample_rate_div100;
    uint8_t audio_channels;
    uint8_t audio_frame_duration_01ms;
    uint16_t payload_limit;
    uint8_t microphone_codec;
    uint16_t microphone_sample_rate_div100;
    uint8_t microphone_channels;
    uint8_t microphone_frame_duration_01ms;
} DsuAvCapabilities;

typedef struct {
    bool (*session_start)(void *context, const DsuAvSessionSettings *settings);
    void (*session_stop)(void *context);
    void (*video_frame)(void *context, uint64_t timestamp_us, uint8_t flags,
        const uint8_t *data, uint32_t length);
    void (*audio_packet)(void *context, uint32_t sequence,
        uint64_t timestamp_us, uint32_t packet_id, uint8_t flags,
        const uint8_t *data, uint16_t length);
} DsuAvCallbacks;

typedef struct {
    uint32_t video_packets;
    uint32_t video_lost;
    uint32_t audio_packets;
    uint32_t audio_lost;
    uint32_t video_queue_delay_us;
    uint32_t audio_queue_delay_us;
    uint16_t recommended_video_bitrate_kbps;
} DsuAvQualityFeedback;

typedef struct {
    int socket_fd;
    uint32_t server_id;
    uint8_t controller_mac[6];
    uint32_t packets_sent;
    uint32_t subscriptions;
    char last_client[48];
    void *av_internal;
    uint32_t av_media_datagrams;
    uint32_t av_media_wrong_source;
    uint32_t av_media_bad_length;
    uint32_t av_media_bad_crc;
    uint32_t av_media_wrong_session;
    uint32_t av_media_video_type;
    uint32_t av_media_audio_type;
    uint32_t av_media_audio_rejected;
} DsuServer;

bool dsu_server_init(DsuServer *server, const DsuServerConfig *config);
void dsu_server_close(DsuServer *server);
void dsu_server_configure_av(DsuServer *server,
    const DsuAvCapabilities *capabilities, const DsuAvCallbacks *callbacks,
    void *callback_context);
void dsu_server_poll(DsuServer *server, const DsuControllerState *state);
void dsu_server_broadcast(DsuServer *server, const DsuControllerState *state);
void dsu_server_expire_clients(DsuServer *server);
bool dsu_server_av_is_active(const DsuServer *server);
const DsuAvSessionSettings *dsu_server_av_settings(const DsuServer *server);
bool dsu_server_av_send_microphone(DsuServer *server, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint16_t length);
void dsu_server_av_set_microphone_state(DsuServer *server, uint8_t state,
    uint8_t reason);
void dsu_server_av_request_keyframe(DsuServer *server, uint32_t last_sequence,
    uint16_t lost_packets, uint8_t reason);
void dsu_server_av_send_quality(DsuServer *server,
    uint32_t interval_ms, const DsuAvQualityFeedback *feedback);
