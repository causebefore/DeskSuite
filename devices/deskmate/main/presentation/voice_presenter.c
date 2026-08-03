/*
 * 文件职责：订阅语音事实事件并维护语音页 View Model。
 */
#include "voice_presenter.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "presentation_dispatch.h"
#include "voice_service.h"

static const char        *TAG = "voice_presenter";
static voice_view_model_t s_view;

static voice_view_state_t state_from_event(int32_t event_id)
{
    switch (event_id)
    {
        case VOICE_SERVICE_EVENT_RECORDING:
            return VOICE_VIEW_STATE_RECORDING;
        case VOICE_SERVICE_EVENT_THINKING:
            return VOICE_VIEW_STATE_THINKING;
        case VOICE_SERVICE_EVENT_SPEAKING:
            return VOICE_VIEW_STATE_SPEAKING;
        case VOICE_SERVICE_EVENT_ERROR:
            return VOICE_VIEW_STATE_ERROR;
        case VOICE_SERVICE_EVENT_NO_SPEECH:
            return VOICE_VIEW_STATE_NO_SPEECH;
        case VOICE_SERVICE_EVENT_CANCELLED:
        case VOICE_SERVICE_EVENT_DONE:
        default:
            return VOICE_VIEW_STATE_IDLE;
    }
}

static void on_voice_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    s_view.state = state_from_event(id);
    s_view.busy =
        id == VOICE_SERVICE_EVENT_RECORDING || id == VOICE_SERVICE_EVENT_THINKING || id == VOICE_SERVICE_EVENT_SPEAKING;
    ESP_LOGI(TAG, "语音呈现状态变更: state=%d busy=%d", (int) s_view.state, (int) s_view.busy);
    (void) presentation_dispatch_status_update();
}

esp_err_t voice_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.state = VOICE_VIEW_STATE_IDLE;
    return esp_event_handler_register(VOICE_SERVICE_EVENT, ESP_EVENT_ANY_ID, on_voice_event, NULL);
}

void voice_presenter_get_view_copy(voice_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
