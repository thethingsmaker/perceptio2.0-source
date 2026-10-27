/*
 * Device bus — how modules announce things without knowing who listens.
 *
 * Three channels, matching what the device reports to the outside world:
 *   bus_event(type, json)   a protocol event: rec_started, upload_retry, fw_updated...  (docs/protocol-v2.md)
 *   bus_state(state)        the device activity: "idle" | "recording" | "uploading"
 *   bus_inventory_changed() the list/progress of stored recordings changed
 * Listeners ("sinks") register once at boot: today the cloud (MQTT); later BLE. A module that announces something
 * includes only this header — it builds and runs with any set of listeners, including none.
 *
 * Calls are forwarded synchronously in the caller's task. Sinks must only queue/flag and return (the cloud sink
 * already does exactly that: the MQTT publish happens in the cloud task). Safe from any task, not from ISRs.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *name;                                         /* for the log */
    void (*on_event)(const char *type, const char *data_json); /* may be NULL */
    void (*on_state)(const char *state);                       /* may be NULL */
    void (*on_inventory)(void);                                /* may be NULL */
} bus_sink_t;

#define BUS_MAX_SINKS 4

esp_err_t   bus_add_sink(const bus_sink_t *sink);   /* the struct must stay valid (static const); ESP_ERR_NO_MEM when full */
void        bus_event(const char *type, const char *data_json);   /* data_json may be NULL */
void        bus_state(const char *state);                         /* repeated identical states are dropped */
void        bus_inventory_changed(void);
const char *bus_get_state(void);
