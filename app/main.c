#include "platform.h"
#include "controller.h"

#include <stdio.h>

#include <dsu/dsu_server.h>

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

    printf("DSU server listening on %s:%d (%s)\n",
        app_platform_ip_address(platform), DSU_PORT,
        app_platform_name(platform));

    DsuControllerState controller = {0};
    AppControllerInput raw_controller;
    while (app_platform_running(platform)) {
        app_platform_update_controller(platform, &raw_controller);
        app_controller_to_dsu(&raw_controller, &controller);
        dsu_server_poll(&server, &controller);
        app_platform_poll_media(platform);
        dsu_server_broadcast(&server, &controller);
        dsu_server_expire_clients(&server);
        app_platform_render_status(platform, &server, &controller);
        app_platform_sleep(platform);
    }

    dsu_server_close(&server);
    app_platform_stop_network(platform);
    app_platform_destroy(platform);
    return 0;
}
