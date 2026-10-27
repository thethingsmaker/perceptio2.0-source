/* Device bus — see include/bus.h. */
#include <string.h>
#include "bus.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "BUS";

static const bus_sink_t *s_sink[BUS_MAX_SINKS];
static int  s_n;
static char s_state[16] = "idle";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t bus_add_sink(const bus_sink_t *sink)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    if (s_n >= BUS_MAX_SINKS) return ESP_ERR_NO_MEM;
    s_sink[s_n++] = sink;                          /* registered at boot, before any announcement: no lock needed */
    ESP_LOGI(TAG, "sink '%s' registered (%d/%d)", sink->name, s_n, BUS_MAX_SINKS);
    return ESP_OK;
}

void bus_event(const char *type, const char *data_json)
{
    ESP_LOGI(TAG, "event %s %.120s%s", type, data_json ? data_json : "", data_json && strlen(data_json) > 120 ? "…" : "");
    for (int i = 0; i < s_n; i++) if (s_sink[i]->on_event) s_sink[i]->on_event(type, data_json);
}

void bus_state(const char *state)
{
    char prev[16];
    bool changed;
    portENTER_CRITICAL(&s_mux);
    changed = strcmp(s_state, state) != 0;
    if (changed) { strlcpy(prev, s_state, sizeof prev); strlcpy(s_state, state, sizeof s_state); }
    portEXIT_CRITICAL(&s_mux);
    if (!changed) return;
    ESP_LOGI(TAG, "state %s -> %s", prev, state);
    for (int i = 0; i < s_n; i++) if (s_sink[i]->on_state) s_sink[i]->on_state(state);
}

void bus_inventory_changed(void)
{
    /* frequent during an upload (progress): not logged */
    for (int i = 0; i < s_n; i++) if (s_sink[i]->on_inventory) s_sink[i]->on_inventory();
}

const char *bus_get_state(void) { return s_state; }
