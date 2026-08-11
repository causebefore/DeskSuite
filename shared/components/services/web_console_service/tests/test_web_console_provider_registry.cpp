#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "web_console_provider_internal.hpp"

static esp_err_t get_status_copy(void *context, web_console_section_status_t *out_status)
{
    (void) context;
    (void) out_status;
    return ESP_OK;
}

int main()
{
    web_console_field_info_t uptime{};
    uptime.id               = "uptime_sec";
    uptime.label            = "Uptime";
    uptime.type             = WEB_CONSOLE_FIELD_TYPE_UINT32;
    uptime.access           = WEB_CONSOLE_FIELD_ACCESS_READ_ONLY;
    uptime.effect           = WEB_CONSOLE_FIELD_EFFECT_NONE;
    uptime.minimum          = 0;
    uptime.maximum          = UINT32_MAX;
    uptime.step             = 1U;
    uptime.format           = "duration_seconds";

    web_console_status_provider_t provider{};
    provider.section_id      = "system";
    provider.label           = "System";
    provider.fields          = &uptime;
    provider.field_count     = 1U;
    provider.get_status_copy = get_status_copy;

    assert(web_console_provider_registry_configure_copy(NULL, 0U, &provider, 1U, NULL, 0U) == ESP_OK);
    const web_console_status_provider_t *stored = web_console_provider_registry_get_status(0U);
    assert(stored != NULL);
    assert(stored->fields != NULL);
    assert(strcmp(stored->fields[0].format, "duration_seconds") == 0);
    web_console_provider_registry_reset();

    uptime.format = "Duration Seconds";
    assert(web_console_provider_registry_configure_copy(NULL, 0U, &provider, 1U, NULL, 0U)
           == ESP_ERR_INVALID_ARG);
    return 0;
}
