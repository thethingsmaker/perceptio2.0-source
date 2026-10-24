/* Task creation with the stack in PSRAM — for tasks that never run with the flash cache disabled (no NVS/OTA writes
 * from them) and are not latency-critical. The TCB stays internal (IDF does that). Falls back to internal RAM. */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static inline BaseType_t task_create_psram(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *handle, BaseType_t core)
{
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, handle, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) r = xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, core);
    return r;
}
