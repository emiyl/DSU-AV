#include <dsu/dsu_server.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#define DSU_PROTOCOL_VERSION 1001
#define DSU_HEADER_SIZE 20
#define DSU_MAX_PACKET_SIZE 128
#define DSU_MAX_CLIENTS 4
#define DSU_SUBSCRIPTION_TIMEOUT_MS 5000ULL

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

bool dsu_server_init(DsuServer *server)
{
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
    server->server_id = (uint32_t)svcGetSystemTick();
    server->controller_mac[0] = 0x02;
    server->controller_mac[1] = 0x00;
    server->controller_mac[2] = 0x44;
    server->controller_mac[3] = (uint8_t)(server->server_id >> 16);
    server->controller_mac[4] = (uint8_t)(server->server_id >> 8);
    server->controller_mac[5] = (uint8_t)server->server_id;
    return true;
}

void dsu_server_close(DsuServer *server)
{
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
        server->socket_fd = -1;
    }
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
    }
}

static uint8_t button_byte(const DsuControllerState *state,
    const uint64_t buttons[8])
{
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (state->buttons & buttons[i])
            result |= (uint8_t)(0x80U >> i);
    }
    return result;
}

void dsu_server_broadcast(DsuServer *server, const DsuControllerState *state)
{
    uint64_t now = clock_ms();
    const uint64_t first_buttons[8] = {
        HidNpadButton_Left, HidNpadButton_Down, HidNpadButton_Right,
        HidNpadButton_Up, HidNpadButton_Plus, HidNpadButton_StickR,
        HidNpadButton_StickL, HidNpadButton_Minus,
    };
    const uint64_t second_buttons[8] = {
        HidNpadButton_Y, HidNpadButton_B, HidNpadButton_A,
        HidNpadButton_X, HidNpadButton_R, HidNpadButton_L,
        HidNpadButton_ZR, HidNpadButton_ZL,
    };

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
        data[16] = button_byte(state, first_buttons);
        data[17] = button_byte(state, second_buttons);
        data[18] = 0;
        data[19] = 0;
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
}
