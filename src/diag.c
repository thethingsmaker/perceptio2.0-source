#include <stdio.h>
#include <stdlib.h>
#include "diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "DIAG";
#define SHOW      8                                   /* tasks listed per line (keeps the line < 256 B for the remote log) */
#define LOW_B     256                                 /* below this: warning */

void diag_stack_report(void)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;   /* margin for tasks created meanwhile */
    TaskStatus_t *ts = heap_caps_malloc(cap * sizeof *ts, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ts) return;
    UBaseType_t n = uxTaskGetSystemState(ts, cap, NULL);
    for (UBaseType_t i = 1; i < n; i++) {             /* insertion sort, ascending headroom (n ≈ 30) */
        TaskStatus_t t = ts[i]; UBaseType_t j = i;
        while (j > 0 && ts[j - 1].usStackHighWaterMark > t.usStackHighWaterMark) { ts[j] = ts[j - 1]; j--; }
        ts[j] = t;
    }
    char line[200]; int len = snprintf(line, sizeof line, "%u tasks, least stack left (B):", (unsigned)n);
    for (UBaseType_t i = 0; i < n && i < SHOW && len < (int)sizeof line - 24; i++)
        len += snprintf(line + len, sizeof line - len, " %.12s %u", ts[i].pcTaskName, (unsigned)ts[i].usStackHighWaterMark);
    if (n && ts[0].usStackHighWaterMark < LOW_B) ESP_LOGW(TAG, "%s  <-- under %d B", line, LOW_B);
    else                                         ESP_LOGI(TAG, "%s", line);
    free(ts);
}
