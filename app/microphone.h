#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dsu/dsu_server.h>

typedef enum {
    APP_MIC_DISABLED = 0,
    APP_MIC_AWAITING_CONSENT = 1,
    APP_MIC_MUTED = 2,
    APP_MIC_CAPTURING = 3,
    APP_MIC_UNAVAILABLE = 4,
} AppMicrophoneState;

typedef struct {
    DsuServer *server;
    bool requested;
    AppMicrophoneState state;
} AppMicrophone;

void app_microphone_init(AppMicrophone *microphone, DsuServer *server);
void app_microphone_session_start(AppMicrophone *microphone);
void app_microphone_session_stop(AppMicrophone *microphone);
void app_microphone_set_state(AppMicrophone *microphone,
    AppMicrophoneState state, uint8_t reason);
bool app_microphone_is_capturing(const AppMicrophone *microphone);
bool app_microphone_is_requested(const AppMicrophone *microphone);
bool app_microphone_send(AppMicrophone *microphone, uint64_t timestamp_us,
    const uint8_t *data, uint16_t length);
