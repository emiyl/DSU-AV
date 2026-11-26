#include "platform.h"
#include "controller.h"

#include <stdio.h>
#include <time.h>

#include <dsu/dsu_server.h>

#define CONTROLLER_BROADCAST_INTERVAL_MS 10ULL

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL +
        (uint64_t)now.tv_nsec / 1000000ULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AppPlatform *platform = app_platform_create();
    if (!platform)
        return 1;
    if (!app_platform_start_network(platform)) {
        fprintf(stderr, "Could not initialize network services.\n");
        app_platform_destroy(platform);
        return 1;
    }

    DsuServerConfig config;
    if (!app_platform_server_config(platform, &config)) {
        fprintf(stderr, "Could not create the DSU server identity.\n");
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        return 1;
    }

    DsuServer server = {0};
    if (!dsu_server_init(&server, &config)) {
        fprintf(stderr, "Could not bind DSU UDP port %d.\n", DSU_PORT);
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        return 1;
    }
    if (!app_platform_attach_server(platform, &server)) {
        fprintf(stderr, "Could not initialize media backend.\n");
        dsu_server_close(&server);
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        return 1;
    }
    dsu_server_configure_av(&server, app_platform_av_capabilities(platform),
        app_platform_av_callbacks(platform),
        app_platform_av_context(platform));

    DsuControllerState controller = {0};
    AppControllerInput raw_controller;
    uint64_t next_broadcast_ms = monotonic_ms();
    while (app_platform_running(platform)) {
        app_platform_update_controller(platform, &raw_controller);
        app_controller_to_dsu(&raw_controller, &controller);
        dsu_server_poll(&server, &controller);
        app_platform_poll_media(platform);
        uint64_t now_ms = monotonic_ms();
        if (now_ms >= next_broadcast_ms) {
            dsu_server_broadcast(&server, &controller);
            next_broadcast_ms = now_ms +
                CONTROLLER_BROADCAST_INTERVAL_MS;
        }
        dsu_server_expire_clients(&server);
        app_platform_render_status(platform, &server, &controller);
        app_platform_sleep(platform);
    }

    dsu_server_close(&server);
    app_platform_stop_network(platform);
    app_platform_destroy(platform);
    return 0;
}
