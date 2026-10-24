/*
 * Remote log: every ESP_LOGx line is captured (esp_log_set_vprintf hook) into a PSRAM ring and shipped by the cloud
 * task as plain text on perceptio/2/<id>/log (QoS 0, ≤ 4 KB per message, newline-separated, IDF line format).
 * Boot lines wait in the ring until the MQTT session is up; while offline the ring keeps the newest ~32 KB.
 * Serial output stays on unless the `log_serial` parameter is false (ship build).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t rlog_init(void);                   /* install the hook; call first thing in app_main */
void      rlog_set_serial(bool on);
void      rlog_set_level(int esp_log_level); /* minimum level shipped remotely (serial keeps the build level) */
/* Cloud task: copy up to `max` bytes of whole lines into out, remove them from the ring. Returns bytes (0 = none). */
size_t    rlog_take(char *out, size_t max);
uint32_t  rlog_dropped(void);                /* lines lost to a full ring */
