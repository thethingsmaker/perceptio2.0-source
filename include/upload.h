/*
 * Resumable HTTPS upload of a stored recording (docs/data-v2.md §2). One upload at a time, in its own task on
 * core 0, started by the backend's `upload` command. The file is streamed from the NAND exactly as written.
 *   GET {url}                      -> received_bytes (resume point)
 *   PUT {url}?offset=N  + X-Chunk-CRC32, chunk_bytes per request, from a PSRAM buffer
 *   POST {url}/complete {size, sha256} -> verified / verified_with_errors / sha_mismatch
 * Network errors: back off 2,4,8..60 s and resume from the server's received_bytes. A recording starting
 * mid-upload aborts the upload (the NAND is the recorder's). Never deletes anything: the backend does, by command.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum { UP_IDLE = 0, UP_RESUMING, UP_SENDING, UP_VERIFYING, UP_BACKOFF } upload_phase_t;

typedef struct {
    upload_phase_t phase;
    char     rec_id[32];
    uint32_t sent_bytes, total_bytes;
    uint32_t retries;
    char     last_error[48];
} upload_status_t;

/* Outcome callback: ok -> result_json is the server's verification object; !ok -> reason (busy, unknown_recording,
 * io_error, network). Called from the upload task. */
typedef void (*upload_done_fn)(const char *cmd_id, bool ok, const char *reason, const char *result_json, const char *stats_json);

esp_err_t upload_init(upload_done_fn done);
/* ESP_ERR_INVALID_STATE: an upload is running or a recording is; ESP_ERR_NOT_FOUND: unknown rec_id. */
esp_err_t upload_start(const char *cmd_id, const char *rec_id, const char *url, const char *token, uint32_t chunk_bytes);
bool      upload_active(void);
void      upload_get_status(upload_status_t *st);
