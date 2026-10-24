/*
 * Cloud link — Perceptio 2.0 device <-> backend control protocol (docs/protocol-v2.md, draft 1).
 * MQTT 5 over WSS via esp-mqtt, on core 0. Control and feedback only; never sample data.
 * Enrolment -> per-racket credential in NVS "p2" -> hello/state/telemetry/event/ack.
 * Tolerates WiFi loss silently; events are buffered (PSRAM ring) and replayed in order.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum { CLOUD_OFF = 0, CLOUD_ENROL_WAIT, CLOUD_ENROL_REJECTED, CLOUD_CONNECTING, CLOUD_ONLINE, CLOUD_OFFLINE } cloud_state_t;

/* What the hello/telemetry need from the rest of the firmware; filled by main's callback. */
typedef struct {
    const char *state;             /* "idle" | "recording" | ... (protocol §9 vocabulary) */
    int      battery_pct;
    bool     charging, plugged;
    int8_t   rssi;
    char     ip[16];
    float    temp_c;
    uint32_t storage_total_mb, storage_free_mb;
    uint32_t heap_kb, psram_kb;
    uint32_t rec_samples, dropped_samples;
    int      odr_hz, xl_fs_g, hg_fs_g;
    float    xl_mg_lsb, gy_mdps_lsb, hg_mg_lsb;
    const char *config_json;       /* current parameter values, for config_hash (may be NULL) */
} cloud_info_t;

typedef void (*cloud_info_fn)(cloud_info_t *out);

typedef struct {
    cloud_state_t state;
    bool     enrolled;             /* credential present in NVS */
    char     device_id[13];
    uint32_t boot_id;
    uint32_t events_pending;
    int64_t  last_hello_utc_ms;
    uint32_t cmds_received;
    char     last_cmd[24];
} cloud_status_t;

esp_err_t   cloud_init(cloud_info_fn info);     /* reads identity/credential, increments boot_id; no network yet */
void        cloud_link(bool up);                /* net link hook: starts/stops the MQTT client */
void        cloud_set_state(const char *state); /* publishes a retained state message when connected */
/* Inventory changed (recording started/stopped/deleted, upload progress): re-publish the retained hello so the
 * broker's stored copy is never stale (spec). Queued; the cloud task sends it. */
void        cloud_inventory_changed(void);
/* Outcome of an `upload` command (from the upload task): acks applied/rejected and refreshes hello. */
void        cloud_upload_done(const char *cmd_id, bool ok, const char *reason, const char *result_json, const char *stats_json);
/* Intentional power-off: retained state {"online":false,"state":"off","reason":...} + "shutdown" event, waits for the
 * PUBACKs (≤ 1.2 s), then disconnects cleanly so the last will does not fire. Returns quickly when offline. */
void        cloud_shutdown(const char *reason);
void        cloud_event(const char *type, const char *data_json);   /* buffered, replayed; data_json may be NULL */
void        cloud_get_status(cloud_status_t *st);
const char *cloud_device_id(void);
const char *cloud_state_name(cloud_state_t s);
