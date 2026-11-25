#include <dsu/dsu_client.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DSU_PROTOCOL_VERSION 1001
#define DSU_HEADER_SIZE 20
#define DSU_MAX_PACKET_SIZE 1280
#define DSU_MEDIA_HEADER_SIZE 28
#define DSU_MSG_VERSION 0x100000U
#define DSU_MSG_PORT_INFO 0x100001U
#define DSU_MSG_PAD_DATA 0x100002U
#define DSU_CAPABILITIES_PAYLOAD_SIZE 29
#define DSU_SESSION_START_PAYLOAD_SIZE 26

typedef struct {
    int socket_fd;
    struct sockaddr_in server_address;
    DsuClientConfig config;
    DsuClientCallbacks callbacks;
    void *callback_context;
    DsuAvCapabilities capabilities;
    DsuAvSessionSettings requested_settings;
    bool capabilities_valid;
    bool capabilities_pending;
    bool session_pending;
    bool media_active;
    uint32_t session_nonce;
    uint32_t session_id;
    uint16_t media_port;
    uint16_t payload_limit;
    uint32_t video_sequence;
    uint32_t video_frame_id;
    uint32_t audio_sequence;
    uint32_t audio_packet_id;
    uint64_t last_subscription_ms;
} DsuClientInternal;

static void write_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        destination[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t read_u16_le(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *source)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)source[i] << (i * 8);
    return value;
}

static float read_float_le(const uint8_t *source)
{
    uint32_t bits = read_u32_le(source);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = i >= 8 && i < 12 ? 0 : data[i];
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t packet_crc32(const uint8_t *packet, size_t length)
{
    return ~crc32_update(0xFFFFFFFFU, packet, length);
}

static DsuClientInternal *client_internal(const DsuClient *client)
{
    return client ? client->internal : NULL;
}

static bool resolve_address(const char *host, uint16_t port,
    struct sockaddr_in *address)
{
    if (!host || !address)
        return false;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
        .ai_protocol = IPPROTO_UDP,
    };
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || !result) {
        if (result)
            freeaddrinfo(result);
        return false;
    }
    memcpy(address, result->ai_addr, sizeof(*address));
    freeaddrinfo(result);
    address->sin_port = htons(port);
    return true;
}

static void finish_packet(uint8_t *packet, uint32_t client_id, uint32_t type,
    size_t payload_length)
{
    memcpy(packet, "DSUC", 4);
    write_u16_le(packet + 4, DSU_PROTOCOL_VERSION);
    write_u16_le(packet + 6, (uint16_t)(payload_length + 4));
    write_u32_le(packet + 8, 0);
    write_u32_le(packet + 12, client_id);
    write_u32_le(packet + 16, type);
    write_u32_le(packet + 8,
        packet_crc32(packet, DSU_HEADER_SIZE + payload_length));
}

static bool send_packet(DsuClientInternal *client, uint32_t type,
    const uint8_t *payload, size_t payload_length, uint16_t port)
{
    uint8_t packet[DSU_MAX_PACKET_SIZE] = {0};
    if (payload_length > sizeof(packet) - DSU_HEADER_SIZE)
        return false;
    if (payload_length)
        memcpy(packet + DSU_HEADER_SIZE, payload, payload_length);
    finish_packet(packet, client->config.client_id, type, payload_length);

    struct sockaddr_in destination = client->server_address;
    if (port)
        destination.sin_port = htons(port);
    return sendto(client->socket_fd, packet,
        DSU_HEADER_SIZE + payload_length, 0,
        (const struct sockaddr *)&destination, sizeof(destination)) ==
        (ssize_t)(DSU_HEADER_SIZE + payload_length);
}

static void send_subscription(DsuClientInternal *client)
{
    uint8_t selection[8] = {0};
    selection[0] = 1;
    selection[1] = client->config.controller_slot;
    send_packet(client, DSU_MSG_PAD_DATA, selection, sizeof(selection),
        client->config.server_port);
}

static void parse_capabilities(DsuClientInternal *client,
    const uint8_t *payload)
{
    bool newly_received = !client->capabilities_valid;
    client->capabilities = (DsuAvCapabilities){
        .flags = read_u32_le(payload + 4),
        .video_codec = payload[8],
        .audio_codec = payload[9],
        .video_width = read_u16_le(payload + 10),
        .video_height = read_u16_le(payload + 12),
        .video_fps = read_u16_le(payload + 14),
        .video_bitrate_kbps = read_u16_le(payload + 16),
        .audio_sample_rate_div100 = read_u16_le(payload + 18),
        .audio_channels = payload[20],
        .audio_frame_duration_01ms = payload[21],
        .payload_limit = read_u16_le(payload + 22),
        .microphone_codec = payload[24],
        .microphone_sample_rate_div100 = read_u16_le(payload + 25),
        .microphone_channels = payload[27],
        .microphone_frame_duration_01ms = payload[28],
    };
    client->capabilities_pending = false;
    client->capabilities_valid = read_u32_le(payload) == DSU_AV_VERSION;
    if (newly_received && client->capabilities_valid &&
        client->callbacks.capabilities)
        client->callbacks.capabilities(client->callback_context,
            &client->capabilities);
}

static bool session_start_pending(DsuClientInternal *client)
{
    uint8_t payload[DSU_SESSION_START_PAYLOAD_SIZE] = {0};
    const DsuAvSessionSettings *settings = &client->requested_settings;
    write_u32_le(payload, client->session_nonce);
    payload[4] = settings->video_codec;
    payload[5] = settings->audio_codec;
    write_u16_le(payload + 6, settings->video_width);
    write_u16_le(payload + 8, settings->video_height);
    write_u16_le(payload + 10, settings->video_fps);
    write_u16_le(payload + 12, settings->video_bitrate_kbps);
    write_u16_le(payload + 14, settings->audio_sample_rate_div100);
    payload[16] = settings->audio_channels;
    payload[17] = settings->audio_frame_duration_01ms;
    write_u16_le(payload + 18, settings->payload_limit);
    payload[20] = settings->microphone_codec;
    write_u16_le(payload + 21, settings->microphone_sample_rate_div100);
    payload[23] = settings->microphone_channels;
    payload[24] = settings->microphone_frame_duration_01ms;
    payload[25] = settings->microphone_consent_mode;
    return send_packet(client, DSU_AV_MSG_SESSION_START, payload,
        sizeof(payload), client->config.server_port);
}

static void close_media(DsuClientInternal *client, uint8_t reason,
    bool notify)
{
    if (!client->media_active && !client->session_pending)
        return;
    client->media_active = false;
    client->session_pending = false;
    if (notify && client->callbacks.media_stopped)
        client->callbacks.media_stopped(client->callback_context, reason);
}

static void handle_controller(DsuClientInternal *client,
    const uint8_t *payload, size_t payload_length)
{
    if (payload_length < 80 || payload[0] != client->config.controller_slot ||
        !payload[1] || !payload[11])
        return;

    DsuControllerState state = {0};
    state.connected = true;
    state.button_masks[0] = payload[16];
    state.button_masks[1] = payload[17];
    state.home_button = payload[18];
    state.touch_button = payload[19];
    state.left_stick_x = payload[20];
    state.left_stick_y = payload[21];
    state.right_stick_x = payload[22];
    state.right_stick_y = payload[23];
    memcpy(state.analog_buttons, payload + 24, 12);
    state.motion_timestamp_us = read_u64_le(payload + 48);
    state.acceleration[0] = read_float_le(payload + 56);
    state.acceleration[1] = read_float_le(payload + 60);
    state.acceleration[2] = read_float_le(payload + 64);
    state.angular_velocity[0] = read_float_le(payload + 68);
    state.angular_velocity[1] = read_float_le(payload + 72);
    state.angular_velocity[2] = read_float_le(payload + 76);
    state.motion_valid = state.motion_timestamp_us != 0;
    if (client->callbacks.controller_state)
        client->callbacks.controller_state(client->callback_context, &state,
            read_u32_le(payload + 12));
}

static void handle_packet(DsuClientInternal *client, const uint8_t *packet,
    size_t length)
{
    uint16_t declared = read_u16_le(packet + 6);
    if (declared < 4 || declared > length - 16 ||
        length != 16U + declared ||
        packet_crc32(packet, length) != read_u32_le(packet + 8))
        return;

    uint32_t type = read_u32_le(packet + 16);
    const uint8_t *payload = packet + DSU_HEADER_SIZE;
    size_t payload_length = declared - 4;

    if (type == DSU_MSG_PAD_DATA) {
        handle_controller(client, payload, payload_length);
    } else if (type == DSU_AV_MSG_CAPABILITIES &&
        payload_length >= DSU_CAPABILITIES_PAYLOAD_SIZE) {
        parse_capabilities(client, payload);
    } else if (type == DSU_AV_MSG_SESSION_START && payload_length >= 16 &&
        client->session_pending &&
        read_u32_le(payload) == client->session_nonce) {
        uint8_t status = payload[12];
        client->session_pending = false;
        if (status == 0) {
            client->media_active = true;
            client->session_id = read_u32_le(payload + 4);
            client->media_port = read_u16_le(payload + 8);
            client->payload_limit = read_u16_le(payload + 10);
            client->video_sequence = 0;
            client->video_frame_id = 0;
            client->audio_sequence = 0;
            client->audio_packet_id = 0;
        }
        if (client->callbacks.media_session)
            client->callbacks.media_session(client->callback_context,
                status == 0, status, &client->requested_settings);
    } else if (type == DSU_AV_MSG_SESSION_STOP && payload_length >= 8 &&
        read_u32_le(payload) == client->session_id) {
        close_media(client, payload[4], true);
    } else if (type == DSU_AV_MSG_KEYFRAME_REQUEST &&
        payload_length >= 12 && read_u32_le(payload) == client->session_id) {
        if (client->callbacks.keyframe_requested)
            client->callbacks.keyframe_requested(client->callback_context,
                read_u32_le(payload + 4), read_u16_le(payload + 8),
                payload[10]);
    } else if (type == DSU_AV_MSG_MICROPHONE_STATE &&
        payload_length >= 8 && read_u32_le(payload) == client->session_id) {
        if (client->callbacks.microphone_state)
            client->callbacks.microphone_state(client->callback_context,
                payload[4], payload[5]);
    } else if (type == DSU_AV_MSG_MICROPHONE &&
        payload_length >= DSU_MEDIA_HEADER_SIZE &&
        read_u32_le(payload) == client->session_id) {
        uint16_t encoded_length = read_u16_le(payload + 26);
        if (encoded_length == payload_length - DSU_MEDIA_HEADER_SIZE &&
            client->callbacks.microphone_packet)
            client->callbacks.microphone_packet(client->callback_context,
                read_u32_le(payload + 4), read_u64_le(payload + 8),
                read_u32_le(payload + 16), payload[24],
                payload + DSU_MEDIA_HEADER_SIZE, encoded_length);
    } else if (type == DSU_AV_MSG_QUALITY_FEEDBACK &&
        payload_length >= 36 && read_u32_le(payload) == client->session_id) {
        if (client->callbacks.quality_feedback)
            client->callbacks.quality_feedback(client->callback_context,
                read_u32_le(payload + 4), read_u32_le(payload + 8),
                read_u32_le(payload + 12), read_u32_le(payload + 16),
                read_u32_le(payload + 20), read_u16_le(payload + 32));
    }
}

bool dsu_client_init(DsuClient *client, const DsuClientConfig *config,
    const DsuClientCallbacks *callbacks, void *callback_context)
{
    if (!client || !config)
        return false;
    memset(client, 0, sizeof(*client));

    DsuClientInternal *internal = calloc(1, sizeof(*internal));
    if (!internal)
        return false;
    internal->socket_fd = -1;
    internal->config = *config;
    if (internal->config.server_port == 0)
        internal->config.server_port = DSU_PORT;
    if (internal->config.client_id == 0)
        internal->config.client_id = (uint32_t)clock_ms() ^ (uint32_t)getpid();
    if (!resolve_address(config->server_address,
            internal->config.server_port, &internal->server_address))
        goto fail;

    internal->socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (internal->socket_fd < 0)
        goto fail;
    int flags = fcntl(internal->socket_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(internal->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;

    struct sockaddr_in bind_address = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(internal->socket_fd,
            (const struct sockaddr *)&bind_address, sizeof(bind_address)) < 0)
        goto fail;
    if (callbacks)
        internal->callbacks = *callbacks;
    internal->callback_context = callback_context;
    client->internal = internal;

    uint8_t port_request[5] = {0};
    write_u32_le(port_request, 1);
    port_request[4] = internal->config.controller_slot;
    send_packet(internal, DSU_MSG_PORT_INFO, port_request,
        sizeof(port_request), internal->config.server_port);
    send_subscription(internal);
    return true;

fail:
    if (internal->socket_fd >= 0)
        close(internal->socket_fd);
    free(internal);
    return false;
}

void dsu_client_close(DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal)
        return;
    dsu_client_stop_media(client, 4);
    close(internal->socket_fd);
    free(internal);
    client->internal = NULL;
}

void dsu_client_poll(DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal)
        return;

    uint64_t now = clock_ms();
    if (now - internal->last_subscription_ms >= 1000) {
        send_subscription(internal);
        if (internal->capabilities_pending && !internal->capabilities_valid)
            send_packet(internal, DSU_AV_MSG_CAPABILITIES, NULL, 0,
                internal->config.server_port);
        if (internal->session_pending)
            session_start_pending(internal);
        internal->last_subscription_ms = now;
    }

    uint8_t packet[DSU_MAX_PACKET_SIZE];
    for (;;) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(internal->socket_fd, packet, sizeof(packet),
            0, (struct sockaddr *)&source, &source_length);
        if (received < 0)
            break;
        if (received < DSU_HEADER_SIZE || source_length != sizeof(source) ||
            source.sin_addr.s_addr != internal->server_address.sin_addr.s_addr ||
            memcmp(packet, "DSUS", 4) != 0)
            continue;
        handle_packet(internal, packet, (size_t)received);
    }
}

bool dsu_client_request_capabilities(DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal)
        return false;
    internal->capabilities_pending = true;
    internal->capabilities_valid = false;
    return send_packet(internal, DSU_AV_MSG_CAPABILITIES, NULL, 0,
        internal->config.server_port);
}

const DsuAvCapabilities *dsu_client_capabilities(const DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    return internal && internal->capabilities_valid
        ? &internal->capabilities : NULL;
}

bool dsu_client_start_media(DsuClient *client,
    const DsuAvSessionSettings *settings)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal || !settings || internal->session_pending ||
        internal->media_active)
        return false;
    internal->requested_settings = *settings;
    internal->session_nonce = (uint32_t)clock_ms() ^
        internal->config.client_id;
    if (internal->session_nonce == 0)
        internal->session_nonce = 1;
    internal->session_pending = true;
    if (!session_start_pending(internal)) {
        internal->session_pending = false;
        return false;
    }
    return true;
}

void dsu_client_stop_media(DsuClient *client, uint8_t reason)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal)
        return;
    if (internal->media_active) {
        uint8_t payload[8] = {0};
        write_u32_le(payload, internal->session_id);
        payload[4] = reason;
        send_packet(internal, DSU_AV_MSG_SESSION_STOP, payload,
            sizeof(payload), internal->config.server_port);
    }
    close_media(internal, reason, true);
}

bool dsu_client_media_active(const DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    return internal && internal->media_active;
}

const DsuAvSessionSettings *dsu_client_media_settings(const DsuClient *client)
{
    DsuClientInternal *internal = client_internal(client);
    return internal && internal->media_active
        ? &internal->requested_settings : NULL;
}

static bool send_media(DsuClientInternal *client, uint32_t type,
    uint32_t sequence, uint64_t timestamp_us, uint32_t media_id,
    uint16_t fragment_index, uint16_t fragment_count, uint8_t flags,
    uint8_t codec, const uint8_t *data, uint16_t length)
{
    uint8_t payload[DSU_MEDIA_HEADER_SIZE + 1280] = {0};
    if (length > client->payload_limit ||
        length > sizeof(payload) - DSU_MEDIA_HEADER_SIZE)
        return false;
    write_u32_le(payload, client->session_id);
    write_u32_le(payload + 4, sequence);
    write_u64_le(payload + 8, timestamp_us);
    write_u32_le(payload + 16, media_id);
    write_u16_le(payload + 20, fragment_index);
    write_u16_le(payload + 22, fragment_count);
    payload[24] = flags;
    payload[25] = codec;
    write_u16_le(payload + 26, length);
    memcpy(payload + DSU_MEDIA_HEADER_SIZE, data, length);
    return send_packet(client, type, payload, DSU_MEDIA_HEADER_SIZE + length,
        client->media_port);
}

bool dsu_client_send_video(DsuClient *client, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint32_t length)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal || !internal->media_active || !data || !length ||
        internal->requested_settings.video_codec == DSU_AV_CODEC_NONE ||
        internal->payload_limit == 0)
        return false;

    uint32_t fragment_count = (length + internal->payload_limit - 1) /
        internal->payload_limit;
    if (fragment_count > UINT16_MAX)
        return false;
    uint32_t frame_id = internal->video_frame_id++;
    uint32_t sent = 0;
    for (uint16_t fragment = 0; fragment < (uint16_t)fragment_count;
        ++fragment) {
        uint32_t remaining = length - sent;
        uint16_t fragment_length = remaining > internal->payload_limit
            ? internal->payload_limit : (uint16_t)remaining;
        uint8_t fragment_flags = flags;
        if (fragment + 1U == fragment_count)
            fragment_flags |= 0x02;
        if (!send_media(internal, DSU_AV_MSG_VIDEO,
                internal->video_sequence++, timestamp_us, frame_id, fragment,
                (uint16_t)fragment_count, fragment_flags,
                internal->requested_settings.video_codec,
                data + sent, fragment_length))
            return false;
        sent += fragment_length;
    }
    return true;
}

bool dsu_client_send_audio(DsuClient *client, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint16_t length)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal || !internal->media_active || !data || !length ||
        internal->requested_settings.audio_codec == DSU_AV_CODEC_NONE)
        return false;
    return send_media(internal, DSU_AV_MSG_AUDIO,
        internal->audio_sequence++, timestamp_us,
        internal->audio_packet_id++, 0, 1, flags,
        internal->requested_settings.audio_codec, data, length);
}

bool dsu_client_send_quality(DsuClient *client, uint32_t interval_ms,
    const DsuAvQualityFeedback *feedback, uint32_t microphone_packets)
{
    DsuClientInternal *internal = client_internal(client);
    if (!internal || !internal->media_active || !feedback)
        return false;
    uint8_t payload[40] = {0};
    write_u32_le(payload, internal->session_id);
    write_u32_le(payload + 4, interval_ms);
    write_u32_le(payload + 8, feedback->video_packets);
    write_u32_le(payload + 12, feedback->video_lost);
    write_u32_le(payload + 16, feedback->audio_packets);
    write_u32_le(payload + 20, feedback->audio_lost);
    write_u32_le(payload + 24, feedback->video_queue_delay_us);
    write_u32_le(payload + 28, feedback->audio_queue_delay_us);
    write_u16_le(payload + 32, feedback->recommended_video_bitrate_kbps);
    write_u32_le(payload + 36, microphone_packets);
    return send_packet(internal, DSU_AV_MSG_QUALITY_FEEDBACK, payload,
        sizeof(payload), internal->config.server_port);
}
