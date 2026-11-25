#include "microphone.h"

void app_microphone_init(AppMicrophone *microphone, DsuServer *server)
{
    microphone->server = server;
    microphone->requested = false;
    microphone->state = APP_MIC_DISABLED;
}

void app_microphone_session_start(AppMicrophone *microphone)
{
    microphone->requested = true;
    app_microphone_set_state(microphone, APP_MIC_AWAITING_CONSENT, 4);
}

void app_microphone_session_stop(AppMicrophone *microphone)
{
    microphone->requested = false;
    microphone->state = APP_MIC_DISABLED;
}

void app_microphone_set_state(AppMicrophone *microphone,
    AppMicrophoneState state, uint8_t reason)
{
    microphone->state = state;
    dsu_server_av_set_microphone_state(microphone->server,
        (uint8_t)state, reason);
}

bool app_microphone_is_capturing(const AppMicrophone *microphone)
{
    return microphone->requested &&
        microphone->state == APP_MIC_CAPTURING;
}

bool app_microphone_is_requested(const AppMicrophone *microphone)
{
    return microphone->requested;
}

bool app_microphone_send(AppMicrophone *microphone, uint64_t timestamp_us,
    const uint8_t *data, uint16_t length)
{
    if (!app_microphone_is_capturing(microphone))
        return false;
    return dsu_server_av_send_microphone(microphone->server,
        timestamp_us, 0, data, length);
}
