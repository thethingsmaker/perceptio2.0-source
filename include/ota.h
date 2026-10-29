/*
 * Firmware update from GitHub Releases (docs: README "Firmware update").
 *
 *   check   GET https://api.github.com/repos/<ota_repo>/releases/latest  (Authorization: Bearer <PAT> when set)
 *           -> tag_name, the first asset whose name ends in ".bin", its API url and size.
 *   install GET <asset api url>  Accept: application/octet-stream, Authorization: Bearer <PAT>
 *           -> 302 to a signed objects.githubusercontent.com URL (fetched WITHOUT the Authorization header: the CDN
 *           rejects two credentials) -> esp_https_ota into the other OTA slot -> reboot.
 *   boot    the new image runs in PENDING_VERIFY; main marks it valid once the cloud connects (or after 90 s if there
 *           is no WiFi); a crash before that rolls back to the previous image (bootloader rollback).
 *
 * Refused while a recording or an upload is running (the NAND is the recorder's; the internal flash must not be
 * written during a session). The PAT lives in NVS key "ota_pat", is never echoed by any API and never logged.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum { OTA_IDLE = 0, OTA_CHECKING, OTA_DOWNLOADING, OTA_VERIFYING, OTA_REBOOTING, OTA_ERROR } ota_phase_t;

typedef struct {
    ota_phase_t phase;
    char     latest_tag[32];        /* from the last successful check ("" = never checked) */
    char     asset_name[64];
    char     asset_url[160];        /* GitHub API asset url */
    uint32_t asset_bytes;
    char     published[24];         /* ISO-8601 from the release */
    bool     checked;               /* latest_* valid */
    bool     update_available;      /* latest_tag != running version */
    uint32_t received_bytes;        /* while downloading */
    char     error[64];             /* last error (phase OTA_ERROR) */
    int64_t  last_check_ms;         /* esp_timer time of the last check attempt */
} ota_status_t;

esp_err_t ota_init(void);                                   /* boot: logs the running slot/state */
/* Update mode. An install needs only WiFi, the web UI (progress) and the OTA task: `enter` runs before the GitHub
 * lookup to switch off what holds internal RAM (main: the cloud link), `leave` runs when the install FAILED (an
 * installed image reboots instead). Both optional; called from the OTA task. */
typedef void (*ota_mode_fn)(void);
void      ota_set_update_mode(ota_mode_fn enter, ota_mode_fn leave);
/* Both run asynchronously in the OTA task; ESP_ERR_INVALID_STATE when busy / recording / uploading, ESP_ERR_NOT_FOUND
 * when install is asked before a successful check. Outcome in ota_get_status(). */
esp_err_t ota_check(void);
esp_err_t ota_install(void);
void      ota_get_status(ota_status_t *st);
bool      ota_busy(void);
/* Rollback handling: true if this image booted as PENDING_VERIFY (first boot after an update). */
bool      ota_pending_verify(void);
void      ota_mark_valid(void);                             /* called by main once the device proved healthy */
/* PAT storage (NVS "ota_pat"): set "" to clear. Never returned in clear: ota_pat_hint gives "ghp_…1234" or "". */
esp_err_t ota_pat_set(const char *pat);
bool      ota_pat_is_set(void);
void      ota_pat_hint(char out[16]);
