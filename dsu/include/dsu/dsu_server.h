#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DSU_PORT 26760

typedef struct {
    bool connected;
    uint64_t buttons;
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
    int socket_fd;
    uint32_t server_id;
    uint8_t controller_mac[6];
    uint32_t packets_sent;
    uint32_t subscriptions;
    char last_client[48];
} DsuServer;

bool dsu_server_init(DsuServer *server);
void dsu_server_close(DsuServer *server);
void dsu_server_poll(DsuServer *server, const DsuControllerState *state);
void dsu_server_broadcast(DsuServer *server, const DsuControllerState *state);
void dsu_server_expire_clients(DsuServer *server);
