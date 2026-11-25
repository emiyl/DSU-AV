#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dsu/dsu_server.h>

typedef struct {
    const char *server_address;
    uint16_t server_port;
    uint32_t client_id;
    uint8_t controller_slot;
} DsuClientConfig;

typedef struct {
    void (*controller_state)(void *context, const DsuControllerState *state,
        uint32_t packet_number);
    void (*capabilities)(void *context,
        const DsuAvCapabilities *capabilities);
    void (*media_session)(void *context, bool started, uint8_t status,
        const DsuAvSessionSettings *settings);
    void (*media_stopped)(void *context, uint8_t reason);
    void (*keyframe_requested)(void *context, uint32_t last_sequence,
        uint16_t lost_packets, uint8_t reason);
    void (*microphone_state)(void *context, uint8_t state, uint8_t reason);
    void (*microphone_packet)(void *context, uint32_t sequence,
        uint64_t timestamp_us, uint32_t packet_id, uint8_t flags,
        const uint8_t *data, uint16_t length);
    void (*quality_feedback)(void *context, uint32_t interval_ms,
        uint32_t video_packets, uint32_t video_lost, uint32_t audio_packets,
        uint32_t audio_lost, uint16_t recommended_video_bitrate_kbps);
} DsuClientCallbacks;

typedef struct {
    void *internal;
} DsuClient;

bool dsu_client_init(DsuClient *client, const DsuClientConfig *config,
    const DsuClientCallbacks *callbacks, void *callback_context);
void dsu_client_close(DsuClient *client);
void dsu_client_poll(DsuClient *client);

bool dsu_client_request_capabilities(DsuClient *client);
const DsuAvCapabilities *dsu_client_capabilities(const DsuClient *client);

bool dsu_client_start_media(DsuClient *client,
    const DsuAvSessionSettings *settings);
void dsu_client_stop_media(DsuClient *client, uint8_t reason);
bool dsu_client_media_active(const DsuClient *client);
const DsuAvSessionSettings *dsu_client_media_settings(
    const DsuClient *client);

bool dsu_client_send_video(DsuClient *client, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint32_t length);
bool dsu_client_send_audio(DsuClient *client, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint16_t length);
bool dsu_client_send_quality(DsuClient *client, uint32_t interval_ms,
    const DsuAvQualityFeedback *feedback, uint32_t microphone_packets);
