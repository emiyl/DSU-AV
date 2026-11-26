#include <dsu/dsu_server.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DSU_PROTOCOL_VERSION 1001
#define DSU_HEADER_SIZE 20
#define DSU_MAX_PACKET_SIZE 1280
#define DSU_MAX_CLIENTS 4
#define DSU_SUBSCRIPTION_TIMEOUT_MS 5000ULL
#define DSU_AV_IDLE_TIMEOUT_MS 5000ULL
#define DSU_AV_SESSION_START_PAYLOAD_SIZE 26
#define DSU_AV_CAPABILITIES_PAYLOAD_SIZE 29
#define DSU_AV_MEDIA_HEADER_SIZE 28
#define DSU_AV_MAX_VIDEO_FRAME (1024 * 1024)

#define DSU_MSG_VERSION 0x100000U
#define DSU_MSG_PORT_INFO 0x100001U
#define DSU_MSG_PAD_DATA 0x100002U

typedef struct {
    struct sockaddr_in address;
    bool active;
    bool subscribed;
    uint64_t last_seen_ms;
    uint64_t last_data_ms;
    uint32_t packet_number;
} DsuClient;

typedef struct {
    DsuAvCapabilities capabilities;
    DsuAvCallbacks callbacks;
    void *callback_context;
    int media_socket_fd;
    bool active;
    uint32_t session_id;
    uint32_t client_nonce;
    struct sockaddr_in client_address;
    DsuAvSessionSettings settings;
    uint16_t media_port;
    uint64_t last_activity_ms;
    uint64_t last_keyframe_request_ms;
    uint32_t microphone_sequence;
    uint32_t microphone_packet_id;
    uint8_t microphone_state;
    uint8_t microphone_reason;
    uint64_t microphone_state_sent_ms;
    uint64_t quality_started_ms;
    uint32_t video_packets;
    uint32_t video_lost;
    uint32_t next_video_sequence;
    bool video_sequence_started;
    uint32_t audio_packets;
    uint32_t audio_lost;
    uint32_t next_audio_sequence;
    bool audio_sequence_started;
    uint8_t *video_frame;
    size_t video_frame_length;
    uint32_t video_frame_id;
    uint16_t video_fragment_count;
    uint16_t video_next_fragment;
    uint64_t video_frame_timestamp_us;
    uint64_t video_fragment_started_ms;
} DsuAvInternal;

static DsuClient g_clients[DSU_MAX_CLIENTS];

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

static void write_float_le(uint8_t *destination, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u32_le(destination, bits);
}

static uint16_t read_u16_le(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static int32_t read_i32_le(const uint8_t *source)
{
    uint32_t value = (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
    int32_t signed_value;
    memcpy(&signed_value, &value, sizeof(signed_value));
    return signed_value;
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

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length,
    size_t zero_offset, size_t zero_length)
{
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = i >= zero_offset && i < zero_offset + zero_length
            ? 0
            : data[i];
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t packet_crc32(const uint8_t *packet, size_t length)
{
    return ~crc32_update(0xFFFFFFFFU, packet, length, 8, 4);
}

static uint64_t clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static void finish_packet(uint8_t *packet, uint32_t server_id, uint32_t type,
    size_t payload_length)
{
    memcpy(packet, "DSUS", 4);
    write_u16_le(packet + 4, DSU_PROTOCOL_VERSION);
    write_u16_le(packet + 6, (uint16_t)(payload_length + 4));
    write_u32_le(packet + 8, 0);
    write_u32_le(packet + 12, server_id);
    write_u32_le(packet + 16, type);
    write_u32_le(packet + 8, packet_crc32(packet, DSU_HEADER_SIZE + payload_length));
}

static bool send_packet(DsuServer *server, const struct sockaddr_in *destination,
    const uint8_t *packet, size_t length)
{
    ssize_t sent = sendto(server->socket_fd, packet, length, 0,
        (const struct sockaddr *)destination, sizeof(*destination));
    if (sent == (ssize_t)length) {
        ++server->packets_sent;
        return true;
    }
    return false;
}

static bool addresses_equal(const struct sockaddr_in *left,
    const struct sockaddr_in *right)
{
    return left->sin_addr.s_addr == right->sin_addr.s_addr &&
        left->sin_port == right->sin_port;
}

static DsuClient *find_or_create_client(const struct sockaddr_in *address)
{
    DsuClient *free_slot = NULL;
    for (unsigned i = 0; i < DSU_MAX_CLIENTS; ++i) {
        if (g_clients[i].active && addresses_equal(&g_clients[i].address, address))
            return &g_clients[i];
        if (!g_clients[i].active && !free_slot)
            free_slot = &g_clients[i];
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->address = *address;
        free_slot->active = true;
    }
    return free_slot;
}

static void set_shared_controller_info(uint8_t *packet, uint8_t slot,
    const DsuServer *server, bool connected)
{
    memset(packet, 0, 11);
    if (!connected)
        return;

    packet[0] = slot;
    packet[1] = 2;
    packet[2] = 2;
    packet[3] = 2;
    memcpy(packet + 4, server->controller_mac, 6);
    packet[10] = 5;
}

static DsuAvInternal *av_internal(const DsuServer *server)
{
    return (DsuAvInternal *)server->av_internal;
}

static void send_av_packet(DsuServer *server,
    const struct sockaddr_in *target, uint32_t type,
    const uint8_t *payload, size_t payload_length);

bool dsu_server_init(DsuServer *server, const DsuServerConfig *config)
{
    if (!config)
        return false;
    memset(server, 0, sizeof(*server));
    memset(g_clients, 0, sizeof(g_clients));
    server->socket_fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return false;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    struct sockaddr_in bind_address = {
        .sin_family = AF_INET,
        .sin_port = htons(DSU_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&bind_address, sizeof(bind_address)) < 0) {
        close(fd);
        return false;
    }

    server->socket_fd = fd;
    server->server_id = config->server_id;
    memcpy(server->controller_mac, config->controller_mac,
        sizeof(server->controller_mac));
    return true;
}

void dsu_server_close(DsuServer *server)
{
    DsuAvInternal *av = av_internal(server);
    if (av) {
        if (av->active) {
            uint8_t payload[8] = {0};
            write_u32_le(payload, av->session_id);
            payload[4] = 4;
            send_av_packet(server, &av->client_address,
                DSU_AV_MSG_SESSION_STOP, payload, sizeof(payload));
        }
        if (av->media_socket_fd >= 0)
            close(av->media_socket_fd);
        if (av->active && av->callbacks.session_stop)
            av->callbacks.session_stop(av->callback_context);
        free(av->video_frame);
        free(av);
        server->av_internal = NULL;
    }
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
        server->socket_fd = -1;
    }
}

void dsu_server_configure_av(DsuServer *server,
    const DsuAvCapabilities *capabilities, const DsuAvCallbacks *callbacks,
    void *callback_context)
{
    DsuAvInternal *old_av = av_internal(server);
    if (old_av) {
        if (old_av->media_socket_fd >= 0)
            close(old_av->media_socket_fd);
        free(old_av->video_frame);
        free(old_av);
    }

    DsuAvInternal *av = calloc(1, sizeof(*av));
    if (!av)
        return;
    av->media_socket_fd = -1;
    if (capabilities)
        av->capabilities = *capabilities;
    av->capabilities.flags |= DSU_AV_CAP_UNAUTHENTICATED;
    if (callbacks)
        av->callbacks = *callbacks;
    av->callback_context = callback_context;
    server->av_internal = av;
}

static void handle_version_request(DsuServer *server,
    const struct sockaddr_in *source)
{
    uint8_t packet[DSU_HEADER_SIZE + 2] = {0};
    write_u16_le(packet + DSU_HEADER_SIZE, DSU_PROTOCOL_VERSION);
    finish_packet(packet, server->server_id, DSU_MSG_VERSION, 2);
    send_packet(server, source, packet, sizeof(packet));
}

static void handle_port_info_request(DsuServer *server,
    const struct sockaddr_in *source, const uint8_t *packet, size_t length,
    bool controller_connected)
{
    if (length < DSU_HEADER_SIZE + 5)
        return;

    const uint8_t *payload = packet + DSU_HEADER_SIZE;
    int32_t count = read_i32_le(payload);
    if (count < 0 || count > 4 ||
        length < DSU_HEADER_SIZE + 4U + (size_t)count)
        return;

    for (int32_t i = 0; i < count; ++i) {
        uint8_t slot = payload[4 + i];
        uint8_t response[DSU_HEADER_SIZE + 12] = {0};
        bool connected = controller_connected && slot == 0;
        if (slot < 4)
            set_shared_controller_info(response + DSU_HEADER_SIZE, slot, server,
                connected);
        finish_packet(response, server->server_id, DSU_MSG_PORT_INFO, 12);
        send_packet(server, source, response, sizeof(response));
    }
}

static void handle_subscription(DsuServer *server,
    const struct sockaddr_in *source, const uint8_t *packet, size_t length)
{
    if (length < DSU_HEADER_SIZE + 8)
        return;

    const uint8_t *payload = packet + DSU_HEADER_SIZE;
    uint8_t flags = payload[0];
    bool selected = flags == 0;
    if ((flags & 1U) != 0 && payload[1] == 0)
        selected = true;
    if ((flags & 2U) != 0 &&
        memcmp(payload + 2, server->controller_mac, 6) == 0)
        selected = true;
    if (!selected)
        return;

    DsuClient *client = find_or_create_client(source);
    if (!client)
        return;
    client->subscribed = true;
    client->last_seen_ms = clock_ms();

    char address[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &source->sin_addr, address, sizeof(address));
    snprintf(server->last_client, sizeof(server->last_client), "%s:%u",
        address, (unsigned)ntohs(source->sin_port));
}

static void send_av_packet(DsuServer *server, const struct sockaddr_in *target,
    uint32_t type, const uint8_t *payload, size_t payload_length)
{
    uint8_t packet[DSU_MAX_PACKET_SIZE] = {0};
    if (payload_length > sizeof(packet) - DSU_HEADER_SIZE)
        return;
    if (payload_length)
        memcpy(packet + DSU_HEADER_SIZE, payload, payload_length);
    finish_packet(packet, server->server_id, type, payload_length);
    send_packet(server, target, packet, DSU_HEADER_SIZE + payload_length);
}

static void handle_av_capabilities(DsuServer *server,
    const struct sockaddr_in *source)
{
    DsuAvInternal *av = av_internal(server);
    uint8_t response[DSU_AV_CAPABILITIES_PAYLOAD_SIZE] = {0};
    if (!av) {
        write_u32_le(response, DSU_AV_VERSION);
        response[0] = 0;
        send_av_packet(server, source, DSU_AV_MSG_CAPABILITIES,
            response, sizeof(response));
        return;
    }

    const DsuAvCapabilities *capabilities = &av->capabilities;
    write_u32_le(response, DSU_AV_VERSION);
    write_u32_le(response + 4, capabilities->flags |
        DSU_AV_CAP_UNAUTHENTICATED);
    response[8] = capabilities->video_codec;
    response[9] = capabilities->audio_codec;
    write_u16_le(response + 10, capabilities->video_width);
    write_u16_le(response + 12, capabilities->video_height);
    write_u16_le(response + 14, capabilities->video_fps);
    write_u16_le(response + 16, capabilities->video_bitrate_kbps);
    write_u16_le(response + 18, capabilities->audio_sample_rate_div100);
    response[20] = capabilities->audio_channels;
    response[21] = capabilities->audio_frame_duration_01ms;
    write_u16_le(response + 22, capabilities->payload_limit);
    response[24] = capabilities->microphone_codec;
    write_u16_le(response + 25, capabilities->microphone_sample_rate_div100);
    response[27] = capabilities->microphone_channels;
    response[28] = capabilities->microphone_frame_duration_01ms;
    send_av_packet(server, source, DSU_AV_MSG_CAPABILITIES,
        response, sizeof(response));
}

static bool open_av_media_socket(DsuAvInternal *av)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return false;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return false;
    }

    socklen_t address_length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &address_length) < 0) {
        close(fd);
        return false;
    }
    av->media_socket_fd = fd;
    av->media_port = ntohs(address.sin_port);
    return true;
}

static void close_av_session(DsuAvInternal *av)
{
    if (av->active && av->callbacks.session_stop)
        av->callbacks.session_stop(av->callback_context);
    if (av->media_socket_fd >= 0) {
        close(av->media_socket_fd);
        av->media_socket_fd = -1;
    }
    av->active = false;
    av->session_id = 0;
    av->microphone_state = 0;
}

static void discard_av_video_frame(DsuAvInternal *av)
{
    av->video_frame_length = 0;
    av->video_frame_id = 0;
    av->video_fragment_count = 0;
    av->video_next_fragment = 0;
}

static bool av_parameters_supported(const DsuAvCapabilities *capabilities,
    const DsuAvSessionSettings *settings)
{
    if (settings->video_codec != DSU_AV_CODEC_NONE &&
        (!(capabilities->flags & DSU_AV_CAP_VIDEO) ||
            settings->video_codec != capabilities->video_codec ||
            settings->video_width == 0 ||
            settings->video_width > capabilities->video_width ||
            settings->video_height == 0 ||
            settings->video_height > capabilities->video_height ||
            settings->video_fps == 0 ||
            settings->video_fps > capabilities->video_fps ||
            settings->video_bitrate_kbps == 0 ||
            settings->video_bitrate_kbps > capabilities->video_bitrate_kbps))
        return false;

    if (settings->audio_codec != DSU_AV_CODEC_NONE &&
        (!(capabilities->flags & DSU_AV_CAP_AUDIO) ||
            settings->audio_codec != capabilities->audio_codec ||
            settings->audio_sample_rate_div100 == 0 ||
            settings->audio_sample_rate_div100 >
                capabilities->audio_sample_rate_div100 ||
            settings->audio_channels == 0 ||
            settings->audio_channels > capabilities->audio_channels ||
            settings->audio_frame_duration_01ms < 25 ||
            settings->audio_frame_duration_01ms > 200))
        return false;

    if (settings->microphone_codec != DSU_AV_CODEC_NONE &&
        (!(capabilities->flags & DSU_AV_CAP_MICROPHONE) ||
            settings->microphone_codec != capabilities->microphone_codec ||
            settings->microphone_sample_rate_div100 == 0 ||
            settings->microphone_sample_rate_div100 >
                capabilities->microphone_sample_rate_div100 ||
            settings->microphone_channels == 0 ||
            settings->microphone_channels > capabilities->microphone_channels))
        return false;
    if (settings->microphone_codec != DSU_AV_CODEC_NONE &&
        (settings->microphone_frame_duration_01ms < 25 ||
            settings->microphone_frame_duration_01ms > 200))
        return false;

    return settings->payload_limit >= DSU_AV_MEDIA_HEADER_SIZE &&
        settings->payload_limit <= capabilities->payload_limit &&
        settings->microphone_consent_mode <= 1;
}

static void send_av_start_response(DsuServer *server,
    const struct sockaddr_in *source, uint32_t nonce, uint32_t session_id,
    uint16_t port, uint16_t payload_limit, uint8_t status)
{
    uint8_t response[16] = {0};
    write_u32_le(response, nonce);
    write_u32_le(response + 4, session_id);
    write_u16_le(response + 8, port);
    write_u16_le(response + 10, payload_limit);
    response[12] = status;
    write_u16_le(response + 14, 5);
    send_av_packet(server, source, DSU_AV_MSG_SESSION_START,
        response, sizeof(response));
}

static void handle_av_session_start(DsuServer *server,
    const struct sockaddr_in *source, const uint8_t *packet, size_t length)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || length < DSU_HEADER_SIZE + DSU_AV_SESSION_START_PAYLOAD_SIZE)
        return;

    const uint8_t *payload = packet + DSU_HEADER_SIZE;
    uint32_t nonce = read_u32_le(payload);
    DsuAvSessionSettings settings = {
        .video_codec = payload[4],
        .audio_codec = payload[5],
        .video_width = read_u16_le(payload + 6),
        .video_height = read_u16_le(payload + 8),
        .video_fps = read_u16_le(payload + 10),
        .video_bitrate_kbps = read_u16_le(payload + 12),
        .audio_sample_rate_div100 = read_u16_le(payload + 14),
        .audio_channels = payload[16],
        .audio_frame_duration_01ms = payload[17],
        .payload_limit = read_u16_le(payload + 18),
        .microphone_codec = payload[20],
        .microphone_sample_rate_div100 = read_u16_le(payload + 21),
        .microphone_channels = payload[23],
        .microphone_frame_duration_01ms = payload[24],
        .microphone_consent_mode = payload[25],
    };

    if (!av_parameters_supported(&av->capabilities, &settings)) {
        send_av_start_response(server, source, nonce, 0, 0, 0, 1);
        return;
    }
    if (av->active) {
        send_av_start_response(server, source, nonce, 0, 0, 0, 2);
        return;
    }
    if (!open_av_media_socket(av)) {
        send_av_start_response(server, source, nonce, 0, 0, 0, 255);
        return;
    }
    if (av->callbacks.session_start &&
        !av->callbacks.session_start(av->callback_context, &settings)) {
        close(av->media_socket_fd);
        av->media_socket_fd = -1;
        send_av_start_response(server, source, nonce, 0, 0, 0, 3);
        return;
    }

    av->active = true;
    av->client_nonce = nonce;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tick = (uint64_t)now.tv_sec * 1000000000ULL +
        (uint64_t)now.tv_nsec;
    av->session_id = (uint32_t)tick ^
        (uint32_t)(tick >> 32) ^ server->server_id ^ nonce;
    if (av->session_id == 0)
        av->session_id = 1;
    av->client_address = *source;
    av->settings = settings;
    av->last_activity_ms = clock_ms();
    av->microphone_state = settings.microphone_codec == DSU_AV_CODEC_NONE ? 0 : 1;
    av->microphone_reason = 4;
    av->microphone_sequence = 0;
    av->microphone_packet_id = 0;
    av->quality_started_ms = clock_ms();
    av->microphone_state_sent_ms = av->quality_started_ms;
    av->video_packets = 0;
    av->video_lost = 0;
    av->video_sequence_started = false;
    av->audio_packets = 0;
    av->audio_lost = 0;
    av->audio_sequence_started = false;
    discard_av_video_frame(av);
    if (settings.video_codec != DSU_AV_CODEC_NONE) {
        if (!av->video_frame)
            av->video_frame = calloc(1, DSU_AV_MAX_VIDEO_FRAME);
        if (!av->video_frame) {
            close_av_session(av);
            send_av_start_response(server, source, nonce, 0, 0, 0, 255);
            return;
        }
    }
    send_av_start_response(server, source, nonce, av->session_id,
        av->media_port, settings.payload_limit, 0);
    if (settings.microphone_codec != DSU_AV_CODEC_NONE) {
        uint8_t microphone_payload[8] = {0};
        write_u32_le(microphone_payload, av->session_id);
        microphone_payload[4] = av->microphone_state;
        microphone_payload[5] = av->microphone_reason;
        send_av_packet(server, source, DSU_AV_MSG_MICROPHONE_STATE,
            microphone_payload, sizeof(microphone_payload));
    }
}

static void handle_av_session_stop(DsuServer *server,
    const struct sockaddr_in *source, const uint8_t *packet, size_t length)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active || length < DSU_HEADER_SIZE + 8 ||
        !addresses_equal(&av->client_address, source) ||
        read_u32_le(packet + DSU_HEADER_SIZE) != av->session_id)
        return;
    close_av_session(av);
}

static void handle_av_quality(DsuServer *server,
    const struct sockaddr_in *source, const uint8_t *packet, size_t length)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active || length < DSU_HEADER_SIZE + 40 ||
        !addresses_equal(&av->client_address, source) ||
        read_u32_le(packet + DSU_HEADER_SIZE) != av->session_id)
        return;
    av->last_activity_ms = clock_ms();
}

static void poll_av_media(DsuServer *server)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active || av->media_socket_fd < 0)
        return;

    uint8_t packet[DSU_MAX_PACKET_SIZE];
    for (;;) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(av->media_socket_fd, packet,
            sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
        if (received < 0)
            break;
        ++server->av_media_datagrams;
        if (received < DSU_HEADER_SIZE + DSU_AV_MEDIA_HEADER_SIZE) {
            ++server->av_media_bad_length;
            continue;
        }
        if (source_length != sizeof(source) ||
            !addresses_equal(&av->client_address, &source) ||
            memcmp(packet, "DSUC", 4) != 0) {
            ++server->av_media_wrong_source;
            continue;
        }

        uint16_t declared_length = read_u16_le(packet + 6);
        if (declared_length < 4U + DSU_AV_MEDIA_HEADER_SIZE ||
            declared_length > DSU_MAX_PACKET_SIZE - 16 ||
            (size_t)received < 16U + declared_length) {
            ++server->av_media_bad_length;
            continue;
        }
        size_t packet_length = 16U + declared_length;
        if (packet_crc32(packet, packet_length) != read_u32_le(packet + 8)) {
            ++server->av_media_bad_crc;
            continue;
        }

        const uint8_t *payload = packet + DSU_HEADER_SIZE;
        if (read_u32_le(payload) != av->session_id) {
            ++server->av_media_wrong_session;
            continue;
        }
        uint16_t encoded_length = read_u16_le(payload + 26);
        if (encoded_length > av->settings.payload_limit ||
            packet_length != (size_t)DSU_HEADER_SIZE +
                DSU_AV_MEDIA_HEADER_SIZE + encoded_length)
            continue;

        uint32_t type = read_u32_le(packet + 16);
        uint32_t sequence = read_u32_le(payload + 4);
        uint64_t timestamp_us = read_u64_le(payload + 8);
        uint32_t media_id = read_u32_le(payload + 16);
        uint16_t fragment_index = read_u16_le(payload + 20);
        uint16_t fragment_count = read_u16_le(payload + 22);
        uint8_t flags = payload[24];
        uint8_t codec = payload[25];
        const uint8_t *encoded = payload + DSU_AV_MEDIA_HEADER_SIZE;
        if (fragment_count == 0 || fragment_index >= fragment_count) {
            ++server->av_media_bad_length;
            continue;
        }
        if (type == DSU_AV_MSG_VIDEO)
            ++server->av_media_video_type;
        else if (type == DSU_AV_MSG_AUDIO)
            ++server->av_media_audio_type;

        if (type == DSU_AV_MSG_VIDEO &&
            codec == av->settings.video_codec && av->video_frame) {
            ++av->video_packets;
            if (av->video_sequence_started &&
                sequence != av->next_video_sequence) {
                ++av->video_lost;
                discard_av_video_frame(av);
                dsu_server_av_request_keyframe(server,
                    av->next_video_sequence, 1, 1);
            }
            av->video_sequence_started = true;
            av->next_video_sequence = sequence + 1;
            if (fragment_index == 0) {
                av->video_frame_id = media_id;
                av->video_frame_length = 0;
                av->video_fragment_count = fragment_count;
                av->video_next_fragment = 0;
                av->video_frame_timestamp_us = timestamp_us;
                av->video_fragment_started_ms = clock_ms();
            }
            if (media_id != av->video_frame_id ||
                fragment_count != av->video_fragment_count ||
                fragment_index != av->video_next_fragment ||
                av->video_frame_length + encoded_length >
                    DSU_AV_MAX_VIDEO_FRAME) {
                ++av->video_lost;
                discard_av_video_frame(av);
                dsu_server_av_request_keyframe(server, sequence, 1, 1);
                continue;
            }
            memcpy(av->video_frame + av->video_frame_length,
                encoded, encoded_length);
            av->video_frame_length += encoded_length;
            ++av->video_next_fragment;
            if ((flags & 0x02) ||
                av->video_next_fragment == av->video_fragment_count) {
                if (av->callbacks.video_frame)
                    av->callbacks.video_frame(av->callback_context,
                        av->video_frame_timestamp_us, flags,
                        av->video_frame, (uint32_t)av->video_frame_length);
                discard_av_video_frame(av);
            }
        } else if (type == DSU_AV_MSG_AUDIO &&
            codec == av->settings.audio_codec &&
            fragment_index == 0 && fragment_count == 1 &&
            av->callbacks.audio_packet) {
            ++av->audio_packets;
            if (av->audio_sequence_started &&
                sequence != av->next_audio_sequence)
                ++av->audio_lost;
            av->audio_sequence_started = true;
            av->next_audio_sequence = sequence + 1;
            av->callbacks.audio_packet(av->callback_context, sequence,
                timestamp_us, media_id, flags, encoded, encoded_length);
        } else {
            if (type == DSU_AV_MSG_AUDIO)
                ++server->av_media_audio_rejected;
            continue;
        }
        av->last_activity_ms = clock_ms();
    }
}

static void service_av_timers(DsuServer *server)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active)
        return;
    uint64_t now = clock_ms();

    if (av->video_fragment_count &&
        now - av->video_fragment_started_ms > 100) {
        ++av->video_lost;
        discard_av_video_frame(av);
        dsu_server_av_request_keyframe(server, 0, 1, 3);
    }
    if (av->settings.microphone_codec != DSU_AV_CODEC_NONE &&
        now - av->microphone_state_sent_ms >= 1000) {
        dsu_server_av_set_microphone_state(server, av->microphone_state,
            0);
        av->microphone_state_sent_ms = now;
    }
    if (now - av->quality_started_ms >= 1000) {
        DsuAvQualityFeedback feedback = {
            .video_packets = av->video_packets,
            .video_lost = av->video_lost,
            .audio_packets = av->audio_packets,
            .audio_lost = av->audio_lost,
        };
        dsu_server_av_send_quality(server,
            (uint32_t)(now - av->quality_started_ms), &feedback);
        av->quality_started_ms = now;
        av->video_packets = 0;
        av->video_lost = 0;
        av->audio_packets = 0;
        av->audio_lost = 0;
    }
}

void dsu_server_poll(DsuServer *server, const DsuControllerState *state)
{
    uint8_t packet[DSU_MAX_PACKET_SIZE];
    struct sockaddr_in source;

    for (;;) {
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(server->socket_fd, packet, sizeof(packet), 0,
            (struct sockaddr *)&source, &source_length);
        if (received < 0)
            break;
        if (received < DSU_HEADER_SIZE || source_length != sizeof(source) ||
            memcmp(packet, "DSUC", 4) != 0)
            continue;

        uint16_t payload_length = read_u16_le(packet + 6);
        if (payload_length < 4 || payload_length > DSU_MAX_PACKET_SIZE - 16 ||
            (size_t)received < 16U + payload_length)
            continue;
        size_t packet_length = 16U + payload_length;
        uint32_t expected_crc = read_u32_le(packet + 8);
        if (packet_crc32(packet, packet_length) != expected_crc)
            continue;

        DsuClient *client = find_or_create_client(&source);
        if (client)
            client->last_seen_ms = clock_ms();

        uint32_t type = read_u32_le(packet + 16);
        if (type == DSU_MSG_VERSION)
            handle_version_request(server, &source);
        else if (type == DSU_MSG_PORT_INFO)
            handle_port_info_request(server, &source, packet, packet_length,
                state->connected);
        else if (type == DSU_MSG_PAD_DATA)
            handle_subscription(server, &source, packet, packet_length);
        else if (type == DSU_AV_MSG_CAPABILITIES)
            handle_av_capabilities(server, &source);
        else if (type == DSU_AV_MSG_SESSION_START)
            handle_av_session_start(server, &source, packet, packet_length);
        else if (type == DSU_AV_MSG_SESSION_STOP)
            handle_av_session_stop(server, &source, packet, packet_length);
        else if (type == DSU_AV_MSG_QUALITY_FEEDBACK)
            handle_av_quality(server, &source, packet, packet_length);
    }
    poll_av_media(server);
    service_av_timers(server);
}

void dsu_server_broadcast(DsuServer *server, const DsuControllerState *state)
{
    uint64_t now = clock_ms();

    for (unsigned i = 0; i < DSU_MAX_CLIENTS; ++i) {
        DsuClient *client = &g_clients[i];
        if (!client->active || !client->subscribed ||
            now - client->last_seen_ms > DSU_SUBSCRIPTION_TIMEOUT_MS)
            continue;

        uint8_t packet[DSU_HEADER_SIZE + 80] = {0};
        uint8_t *data = packet + DSU_HEADER_SIZE;
        set_shared_controller_info(data, 0, server, state->connected);
        data[11] = state->connected ? 1 : 0;
        write_u32_le(data + 12, ++client->packet_number);
        data[16] = state->button_masks[0];
        data[17] = state->button_masks[1];
        data[18] = state->home_button;
        data[19] = state->touch_button;
        data[20] = state->left_stick_x;
        data[21] = state->left_stick_y;
        data[22] = state->right_stick_x;
        data[23] = state->right_stick_y;
        memcpy(data + 24, state->analog_buttons, 12);
        write_u64_le(data + 48, state->motion_timestamp_us);
        if (state->motion_valid) {
            write_float_le(data + 56, state->acceleration[0]);
            write_float_le(data + 60, state->acceleration[1]);
            write_float_le(data + 64, state->acceleration[2]);
            write_float_le(data + 68, state->angular_velocity[0]);
            write_float_le(data + 72, state->angular_velocity[1]);
            write_float_le(data + 76, state->angular_velocity[2]);
        }

        finish_packet(packet, server->server_id, DSU_MSG_PAD_DATA, 80);
        if (send_packet(server, &client->address, packet, sizeof(packet)))
            client->last_data_ms = now;
    }
}

bool dsu_server_av_is_active(const DsuServer *server)
{
    DsuAvInternal *av = av_internal(server);
    return av && av->active;
}

const DsuAvSessionSettings *dsu_server_av_settings(const DsuServer *server)
{
    DsuAvInternal *av = av_internal(server);
    return av && av->active ? &av->settings : NULL;
}

bool dsu_server_av_send_microphone(DsuServer *server, uint64_t timestamp_us,
    uint8_t flags, const uint8_t *data, uint16_t length)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active || av->media_socket_fd < 0 ||
        av->microphone_state != 3 ||
        av->settings.microphone_codec != DSU_AV_CODEC_OPUS ||
        length > av->settings.payload_limit)
        return false;

    uint8_t packet[DSU_MAX_PACKET_SIZE] = {0};
    uint8_t *payload = packet + DSU_HEADER_SIZE;
    write_u32_le(payload, av->session_id);
    write_u32_le(payload + 4, av->microphone_sequence++);
    write_u64_le(payload + 8, timestamp_us);
    write_u32_le(payload + 16, av->microphone_packet_id++);
    write_u16_le(payload + 20, 0);
    write_u16_le(payload + 22, 1);
    payload[24] = flags;
    payload[25] = DSU_AV_CODEC_OPUS;
    write_u16_le(payload + 26, length);
    memcpy(payload + DSU_AV_MEDIA_HEADER_SIZE, data, length);
    finish_packet(packet, server->server_id, DSU_AV_MSG_MICROPHONE,
        DSU_AV_MEDIA_HEADER_SIZE + length);
    ssize_t sent = sendto(av->media_socket_fd, packet,
        DSU_HEADER_SIZE + DSU_AV_MEDIA_HEADER_SIZE + length, 0,
        (const struct sockaddr *)&av->client_address,
        sizeof(av->client_address));
    if (sent >= 0) {
        ++server->packets_sent;
        return true;
    }
    return false;
}

void dsu_server_av_set_microphone_state(DsuServer *server, uint8_t state,
    uint8_t reason)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active)
        return;
    av->microphone_state = state;
    av->microphone_reason = reason;
    uint8_t payload[8] = {0};
    write_u32_le(payload, av->session_id);
    payload[4] = state;
    payload[5] = reason;
    send_av_packet(server, &av->client_address,
        DSU_AV_MSG_MICROPHONE_STATE, payload, sizeof(payload));
}

void dsu_server_av_request_keyframe(DsuServer *server, uint32_t last_sequence,
    uint16_t lost_packets, uint8_t reason)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active)
        return;
    uint64_t now = clock_ms();
    if (av->last_keyframe_request_ms &&
        now - av->last_keyframe_request_ms < 250)
        return;
    uint8_t payload[12] = {0};
    write_u32_le(payload, av->session_id);
    write_u32_le(payload + 4, last_sequence);
    write_u16_le(payload + 8, lost_packets);
    payload[10] = reason;
    send_av_packet(server, &av->client_address,
        DSU_AV_MSG_KEYFRAME_REQUEST, payload, sizeof(payload));
    av->last_keyframe_request_ms = now;
}

void dsu_server_av_send_quality(DsuServer *server, uint32_t interval_ms,
    const DsuAvQualityFeedback *feedback)
{
    DsuAvInternal *av = av_internal(server);
    if (!av || !av->active || !feedback)
        return;
    uint8_t payload[40] = {0};
    write_u32_le(payload, av->session_id);
    write_u32_le(payload + 4, interval_ms);
    write_u32_le(payload + 8, feedback->video_packets);
    write_u32_le(payload + 12, feedback->video_lost);
    write_u32_le(payload + 16, feedback->audio_packets);
    write_u32_le(payload + 20, feedback->audio_lost);
    write_u32_le(payload + 24, feedback->video_queue_delay_us);
    write_u32_le(payload + 28, feedback->audio_queue_delay_us);
    write_u16_le(payload + 32, feedback->recommended_video_bitrate_kbps);
    send_av_packet(server, &av->client_address, DSU_AV_MSG_QUALITY_FEEDBACK,
        payload, sizeof(payload));
}

void dsu_server_expire_clients(DsuServer *server)
{
    uint64_t now = clock_ms();
    unsigned active_subscriptions = 0;
    bool have_client = false;

    for (unsigned i = 0; i < DSU_MAX_CLIENTS; ++i) {
        DsuClient *client = &g_clients[i];
        if (client->active &&
            now - client->last_seen_ms > DSU_SUBSCRIPTION_TIMEOUT_MS) {
            memset(client, 0, sizeof(*client));
            continue;
        }
        if (client->active) {
            have_client = true;
            if (client->subscribed)
                ++active_subscriptions;
        }
    }
    server->subscriptions = active_subscriptions;
    if (!have_client)
        server->last_client[0] = '\0';

    DsuAvInternal *av = av_internal(server);
    if (av && av->active &&
        now - av->last_activity_ms > DSU_AV_IDLE_TIMEOUT_MS) {
        uint8_t stop_payload[8] = {0};
        write_u32_le(stop_payload, av->session_id);
        stop_payload[4] = 2;
        send_av_packet(server, &av->client_address,
            DSU_AV_MSG_SESSION_STOP, stop_payload, sizeof(stop_payload));
        close_av_session(av);
    }
}
