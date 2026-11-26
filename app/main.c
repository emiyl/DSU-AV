#include "platform.h"
#include "controller.h"
#include "logging.h"

#include <stdio.h>
#include <time.h>

#include <dsu/dsu_server.h>

static uint64_t log_clock_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL +
        (uint64_t)now.tv_nsec / 1000ULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!app_logging_init())
        fprintf(stderr, "Could not open the controller log file.\n");

    AppPlatform *platform = app_platform_create();
    if (!platform) {
        app_logging_close();
        return 1;
    }
    if (!app_platform_start_network(platform)) {
        app_logging_write("Could not initialize network services.");
        fprintf(stderr, "Could not initialize network services.\n");
        app_platform_destroy(platform);
        app_logging_close();
        return 1;
    }

    DsuServerConfig config;
    if (!app_platform_server_config(platform, &config)) {
        app_logging_write("Could not create the DSU server identity.");
        fprintf(stderr, "Could not create the DSU server identity.\n");
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        app_logging_close();
        return 1;
    }

    DsuServer server = {0};
    if (!dsu_server_init(&server, &config)) {
        app_logging_write("Could not bind DSU UDP port %d.", DSU_PORT);
        fprintf(stderr, "Could not bind DSU UDP port %d.\n", DSU_PORT);
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        app_logging_close();
        return 1;
    }
    if (!app_platform_attach_server(platform, &server)) {
        app_logging_write("Could not initialize media backend.");
        fprintf(stderr, "Could not initialize media backend.\n");
        dsu_server_close(&server);
        app_platform_stop_network(platform);
        app_platform_destroy(platform);
        app_logging_close();
        return 1;
    }
    dsu_server_configure_av(&server, app_platform_av_capabilities(platform),
        app_platform_av_callbacks(platform),
        app_platform_av_context(platform));

    printf("DSU server listening on %s:%d (%s)\n",
        app_platform_ip_address(platform), DSU_PORT,
        app_platform_name(platform));
    app_logging_write("DSU server listening on %s:%d (%s).",
        app_platform_ip_address(platform), DSU_PORT,
        app_platform_name(platform));

    DsuControllerState controller = {0};
    AppControllerInput raw_controller;
    uint64_t next_media_log_us = 0;
    while (app_platform_running(platform)) {
        app_platform_update_controller(platform, &raw_controller);
        app_controller_to_dsu(&raw_controller, &controller);
        dsu_server_poll(&server, &controller);
        app_platform_poll_media(platform);
        dsu_server_broadcast(&server, &controller);
        dsu_server_expire_clients(&server);
        uint64_t now_us = log_clock_us();
        if (now_us >= next_media_log_us) {
            app_logging_write(
                "DSU media receive: datagrams=%u video_type=%u audio_type=%u "
                "wrong_source=%u bad_length=%u bad_crc=%u wrong_session=%u "
                "audio_rejected=%u.",
                server.av_media_datagrams, server.av_media_video_type,
                server.av_media_audio_type, server.av_media_wrong_source,
                server.av_media_bad_length, server.av_media_bad_crc,
                server.av_media_wrong_session, server.av_media_audio_rejected);
            next_media_log_us = now_us + 1000000;
        }
        app_platform_render_status(platform, &server, &controller);
        app_platform_sleep(platform);
    }

    dsu_server_close(&server);
    app_platform_stop_network(platform);
    app_platform_destroy(platform);
    app_logging_close();
    return 0;
}
