/*
 * Command layer — the one place where device commands are decided.
 *
 * Every way of controlling the device (web UI / HTTP, backend / MQTT, the button, later BLE) translates its own
 * message format into one of these calls and its result back into its own format. The rules (what is allowed when,
 * the checks, the side effects, the log line) exist once, here. Transports keep only what is theirs: parsing,
 * acknowledgements, status codes.
 *
 * Each call logs exactly one line:  APP: <command> by <who> -> ok | <reason>
 * Call from task context (HTTP, cloud and main tasks), never from an ISR.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "store.h"

typedef enum {
    APP_OK = 0,
    APP_BUSY,               /* recording already running / NAND in use */
    APP_NOT_RECORDING,
    APP_STORAGE_FULL,
    APP_BATTERY_LOW,        /* < 5 % on battery (no cable): a session would die early */
    APP_BAD_ARGS,
    APP_UNKNOWN_RECORDING,
    APP_ERROR,              /* NAND absent or failed */
} app_err_t;

const char *app_reason(app_err_t e);      /* protocol-v2 reason strings: "busy", "not_recording", ... ("" for APP_OK) */

#define APP_TAG_MAX 64

app_err_t app_rec_start(const char *rec_tag, const char *by, char rec_id_out[32]);   /* rec_tag may be NULL */
/* expect_rec_id: NULL = stop whatever runs; otherwise APP_BAD_ARGS if a different recording is running. out may be NULL. */
app_err_t app_rec_stop(const char *by, const char *expect_rec_id, store_rec_t *out);
app_err_t app_rec_toggle(const char *by);                                             /* button: start or stop */
app_err_t app_rec_delete(const char *rec_id, const char *by);
app_err_t app_rec_generate(uint32_t minutes, const char *rec_tag, const char *by);    /* synthetic test file */
void      app_identify(uint32_t seconds, const char *by);
/* Restart / power off. The caller answers its client first (both stop this device's tasks). app_reboot does not
 * return; app_power_off hands over to the main loop (closes a running session, then ship mode / CHARGE-IDLE). */
void      app_reboot(const char *by);
void      app_power_off(const char *by);
