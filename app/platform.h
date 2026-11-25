#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dsu/dsu_server.h>

typedef struct AppPlatform AppPlatform;

AppPlatform *app_platform_create(void);
void app_platform_destroy(AppPlatform *platform);

bool app_platform_running(AppPlatform *platform);
bool app_platform_start_network(AppPlatform *platform);
void app_platform_stop_network(AppPlatform *platform);
bool app_platform_attach_server(AppPlatform *platform, DsuServer *server);
bool app_platform_server_config(AppPlatform *platform,
    DsuServerConfig *config);
const char *app_platform_ip_address(const AppPlatform *platform);
const char *app_platform_name(const AppPlatform *platform);

void app_platform_update_controller(AppPlatform *platform,
    DsuControllerState *controller);
const DsuAvCapabilities *app_platform_av_capabilities(
    const AppPlatform *platform);
const DsuAvCallbacks *app_platform_av_callbacks(const AppPlatform *platform);
void *app_platform_av_context(AppPlatform *platform);
void app_platform_poll_media(AppPlatform *platform);
void app_platform_render_status(AppPlatform *platform,
    const DsuServer *server, const DsuControllerState *controller);
void app_platform_sleep(AppPlatform *platform);
