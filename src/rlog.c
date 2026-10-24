#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "rlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"

#define RING 32768
static char *s_ring;                          /* PSRAM */
static volatile uint32_t s_head, s_tail;      /* byte counters; head - tail = used */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static bool s_serial = true;
static int  s_level = ESP_LOG_INFO;
static uint32_t s_dropped;

/* RTC-memory tail: survives panics, watchdog and software resets (not a power cut). Published at the next boot when
 * the reset was a crash, so the lines before a crash reach the backend without a serial monitor. */
#define RTC_RING 4096
#define RTC_MAGIC 0x524C4F47u                  /* "RLOG" */
static RTC_NOINIT_ATTR char     s_rtc[RTC_RING];
static RTC_NOINIT_ATTR uint32_t s_rtc_head, s_rtc_magic;

static void ring_put_remote_only(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    while (s_head - s_tail + n > RING) {
        uint32_t t = s_tail;
        while (t != s_head && s_ring[t % RING] != '\n') t++;
        s_tail = (t == s_head) ? t : t + 1;
        s_dropped++;
    }
    for (size_t i = 0; i < n; i++) s_ring[(s_head + i) % RING] = s[i];
    s_head += n;
    portEXIT_CRITICAL(&s_mux);
}

static void ring_put(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) s_rtc[(s_rtc_head + i) % RTC_RING] = s[i];
    s_rtc_head += n;
    while (s_head - s_tail + n > RING) {              /* full: drop whole oldest lines */
        uint32_t t = s_tail;
        while (t != s_head && s_ring[t % RING] != '\n') t++;
        s_tail = (t == s_head) ? t : t + 1;
        s_dropped++;
    }
    for (size_t i = 0; i < n; i++) s_ring[(s_head + i) % RING] = s[i];
    s_head += n;
    portEXIT_CRITICAL(&s_mux);
}

/* IDF line format: "I (12345) TAG: message\n" — the level letter is the first char (after an optional colour escape) */
static int hook(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n < 0) return 0;
    if ((size_t)n >= sizeof line) { n = sizeof line - 1; line[n - 1] = '\n'; }
    const char *p = line; if (p[0] == 0x1b) { p = strchr(p, 'm'); p = p ? p + 1 : line; }   /* strip colour prefix */
    int lvl = p[0] == 'E' ? ESP_LOG_ERROR : p[0] == 'W' ? ESP_LOG_WARN : p[0] == 'I' ? ESP_LOG_INFO : p[0] == 'D' ? ESP_LOG_DEBUG : ESP_LOG_VERBOSE;
    if (s_ring && lvl <= s_level && !xPortInIsrContext()) {
        size_t len = strlen(p);
        const char *e = strstr(p, "\x1b[0m");                               /* colour suffix (CONFIG_LOG_COLORS): drop it, keep the newline */
        if (e) len = e - p;
        ring_put(p, len);
        if (e) ring_put("\n", 1);
    }
    return s_serial && s_prev ? s_prev(fmt, ap) : n;
}

esp_err_t rlog_init(void)
{
    s_ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;
    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_CPU_LOCKUP;
    if (s_rtc_magic == RTC_MAGIC && crashed && s_rtc_head) {
        /* replay the tail of the previous boot into the remote ring, oldest first, whole lines only */
        uint32_t n = s_rtc_head < RTC_RING ? s_rtc_head : RTC_RING, start = s_rtc_head - n;
        while (n && s_rtc[start % RTC_RING] != '\n') { start++; n--; }
        if (n) start++, n--;
        char hdr[64]; int h = snprintf(hdr, sizeof hdr, "--- %lu bytes of log before the previous reset (reason %d) ---\n", (unsigned long)n, (int)rr);
        ring_put_remote_only(hdr, h);
        for (uint32_t i = 0; i < n; i++) { char c = s_rtc[(start + i) % RTC_RING]; ring_put_remote_only(&c, 1); }
        ring_put_remote_only("--- end of previous boot ---\n", 29);
    }
    s_rtc_magic = RTC_MAGIC; s_rtc_head = 0;
    s_prev = esp_log_set_vprintf(hook);
    return ESP_OK;
}
void rlog_set_serial(bool on) { s_serial = on; }
void rlog_set_level(int l) { s_level = l; }
uint32_t rlog_dropped(void) { return s_dropped; }

size_t rlog_take(char *out, size_t max)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t avail = s_head - s_tail, n = avail < max ? avail : (uint32_t)max;
    if (n < avail) { while (n && s_ring[(s_tail + n - 1) % RING] != '\n') n--; }   /* whole lines only */
    for (uint32_t i = 0; i < n; i++) out[i] = s_ring[(s_tail + i) % RING];
    s_tail += n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
