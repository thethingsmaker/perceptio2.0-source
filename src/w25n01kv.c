#include <string.h>
#include "w25n01kv.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "NAND";

#define CMD_RESET       0xFF
#define CMD_JEDEC       0x9F
#define CMD_RD_SR       0x0F
#define CMD_WR_SR       0x1F
#define CMD_WREN        0x06
#define CMD_BLK_ERASE   0xD8
#define CMD_LOAD_DATA   0x02
#define CMD_PROG_EXEC   0x10
#define CMD_PAGE_READ   0x13
#define CMD_READ        0x03
#define CMD_FAST_READ   0x0B
#define SR_PROT   0xA0
#define SR_CONF   0xB0
#define SR_STAT   0xC0
#define ST_BUSY   0x01
#define ST_EFAIL  0x04
#define ST_PFAIL  0x08

static spi_device_handle_t s_dev;
static nand_info_t s_info;
static uint8_t s_bad[NAND_BLOCKS / 8];
static SemaphoreHandle_t s_lock;
static uint8_t WORD_ALIGNED_ATTR s_tx[NAND_PAGE_SIZE + NAND_SPARE_SIZE + 8];
static uint8_t WORD_ALIGNED_ATTR s_rx[NAND_PAGE_SIZE + NAND_SPARE_SIZE + 8];

/* full duplex: send txlen bytes, then clock rxlen more; rx = tail */
static esp_err_t xfer(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen)
{
    size_t n = txlen + rxlen;
    if (n > sizeof s_tx) return ESP_ERR_INVALID_SIZE;
    memcpy(s_tx, tx, txlen); memset(s_tx + txlen, 0, rxlen);
    spi_transaction_t t = { .length = 8 * n, .tx_buffer = s_tx, .rx_buffer = s_rx };
    esp_err_t e = spi_device_transmit(s_dev, &t);
    if (rx && rxlen) memcpy(rx, s_rx + txlen, rxlen);
    return e;
}
static uint8_t sr(uint8_t a) { uint8_t tx[2] = { CMD_RD_SR, a }, v = 0; xfer(tx, 2, &v, 1); return v; }
static void wsr(uint8_t a, uint8_t v) { uint8_t tx[3] = { CMD_WR_SR, a, v }; xfer(tx, 3, NULL, 0); }
static void wren(void) { uint8_t c = CMD_WREN; xfer(&c, 1, NULL, 0); }
static bool wait_ready(uint8_t *status)
{
    uint8_t s = 0;
    for (int i = 0; i < 20000; i++) {                 /* ≤200 ms */
        s = sr(SR_STAT);
        if (!(s & ST_BUSY)) { if (status) *status = s; return true; }
        esp_rom_delay_us(10);
    }
    if (status) *status = s;
    return false;
}
static esp_err_t page_to_buf(uint32_t page)           /* 13h: array -> data buffer */
{
    uint8_t c[4] = { CMD_PAGE_READ, 0x00, page >> 8, page & 0xFF };
    xfer(c, 4, NULL, 0);
    return wait_ready(NULL) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint8_t nand_status(uint8_t addr) { return sr(addr); }
const nand_info_t *nand_info(void) { return &s_info; }
bool nand_block_is_bad(uint16_t blk) { return blk < NAND_BLOCKS && (s_bad[blk / 8] & (1 << (blk % 8))); }

esp_err_t nand_init(uint32_t clock_hz)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_dev) spi_bus_remove_device(s_dev);
    spi_device_interface_config_t dev = {
        .clock_speed_hz = clock_hz, .mode = 0, .spics_io_num = PIN_NAND_CS, .queue_size = 2, .input_delay_ns = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_dev));
    s_info.clock_hz = clock_hz;

    uint8_t rst = CMD_RESET; xfer(&rst, 1, NULL, 0); vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t tx[2] = { CMD_JEDEC, 0x00 };
    xfer(tx, 2, s_info.jedec, 3);
    if (s_info.jedec[0] != 0xEF || s_info.jedec[1] != 0xAE || s_info.jedec[2] != 0x21) {
        ESP_LOGE(TAG, "JEDEC %02X %02X %02X (expect EF AE 21)", s_info.jedec[0], s_info.jedec[1], s_info.jedec[2]);
        return ESP_ERR_NOT_FOUND;
    }
    wren(); wsr(SR_PROT, 0x00);                        /* clear BP[3:0]/TB — array is write-protected at power-up */
    s_info.sr1 = sr(SR_PROT); s_info.sr2 = sr(SR_CONF);
    if (s_info.sr1 & 0x7C) ESP_LOGW(TAG, "block protection still set: SR1=%02X", s_info.sr1);

    /* factory bad-block scan: spare byte 0 of page 0 of each block != 0xFF */
    memset(s_bad, 0, sizeof s_bad);
    s_info.bad_blocks = 0;
    for (int b = 0; b < NAND_BLOCKS; b++) {
        uint8_t m = 0xFF;
        if (page_to_buf(b * NAND_PAGES_PER_BLK) == ESP_OK) {
            uint8_t c[4] = { CMD_READ, NAND_PAGE_SIZE >> 8, NAND_PAGE_SIZE & 0xFF, 0x00 };
            xfer(c, 4, &m, 1);
        }
        if (m != 0xFF) { s_bad[b / 8] |= 1 << (b % 8); s_info.bad_blocks++; ESP_LOGW(TAG, "bad block %d (marker %02X)", b, m); }
    }
    ESP_LOGI(TAG, "W25N01KV %lu MHz, SR1 %02X SR2 %02X, %d bad block(s), %d usable (%u MB)",
             (unsigned long)clock_hz / 1000000, s_info.sr1, s_info.sr2, s_info.bad_blocks,
             NAND_BLOCKS - s_info.bad_blocks, (unsigned)((NAND_BLOCKS - s_info.bad_blocks) * NAND_BLOCK_SIZE >> 20));
    return ESP_OK;
}

void nand_mark_bad(uint16_t blk)
{
    if (blk >= NAND_BLOCKS) return;
    s_bad[blk / 8] |= 1 << (blk % 8);
    s_info.bad_blocks++;
    /* write marker 0x00 at spare byte 0 of page 0 (column 2048) — survives power loss */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t load[4] = { CMD_LOAD_DATA, NAND_PAGE_SIZE >> 8, NAND_PAGE_SIZE & 0xFF, 0x00 };
    wren(); xfer(load, 4, NULL, 0);
    uint32_t page = blk * NAND_PAGES_PER_BLK;
    uint8_t c[4] = { CMD_PROG_EXEC, 0x00, page >> 8, page & 0xFF };
    xfer(c, 4, NULL, 0); wait_ready(NULL);
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "block %u marked bad", blk);
}

esp_err_t nand_block_erase(uint16_t blk)
{
    if (blk >= NAND_BLOCKS) return ESP_ERR_INVALID_ARG;
    uint32_t page = blk * NAND_PAGES_PER_BLK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wren();
    uint8_t c[4] = { CMD_BLK_ERASE, 0x00, page >> 8, page & 0xFF };
    xfer(c, 4, NULL, 0);
    uint8_t st; bool ok = wait_ready(&st);
    xSemaphoreGive(s_lock);
    if (!ok) return ESP_ERR_TIMEOUT;
    return (st & ST_EFAIL) ? ESP_FAIL : ESP_OK;
}

esp_err_t nand_page_program(uint32_t page, const uint8_t *data, size_t len)
{
    if (page >= NAND_PAGES || len == 0 || len > NAND_PAGE_SIZE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 02h load (column 0, resets the buffer to 0xFF before loading) then 10h execute.
     * wren() first: it goes through xfer() and would overwrite s_tx[0]. */
    wren();
    s_tx[0] = CMD_LOAD_DATA; s_tx[1] = 0; s_tx[2] = 0; memcpy(s_tx + 3, data, len);
    spi_transaction_t t = { .length = 8 * (3 + len), .tx_buffer = s_tx };
    spi_device_transmit(s_dev, &t);
    uint8_t c[4] = { CMD_PROG_EXEC, 0x00, page >> 8, page & 0xFF };
    xfer(c, 4, NULL, 0);
    uint8_t st; bool ok = wait_ready(&st);
    xSemaphoreGive(s_lock);
    if (!ok) return ESP_ERR_TIMEOUT;
    return (st & ST_PFAIL) ? ESP_FAIL : ESP_OK;
}

esp_err_t nand_page_read(uint32_t page, uint16_t col, uint8_t *dst, size_t len, int *ecc_status)
{
    if (page >= NAND_PAGES || col + len > NAND_PAGE_SIZE + NAND_SPARE_SIZE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = page_to_buf(page);
    if (e == ESP_OK) {
        int ecc = (sr(SR_STAT) >> 4) & 3;
        if (ecc_status) *ecc_status = ecc;
        /* 03h is limited to 50 MHz; 0Bh (8 dummy clocks) is good to 104 MHz — pick by clock */
        uint8_t c[4] = { s_info.clock_hz > 30000000 ? CMD_FAST_READ : CMD_READ, col >> 8, col & 0xFF, 0x00 };
        e = xfer(c, 4, dst, len);
        if (ecc >= 2) e = ESP_ERR_INVALID_CRC;   /* 10 = uncorrectable, 11 = reserved/multi-page */
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t nand_spare_read(uint32_t page, uint8_t *dst64)
{
    return nand_page_read(page, NAND_PAGE_SIZE, dst64, NAND_SPARE_SIZE, NULL);
}
