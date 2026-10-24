/*
 * Winbond W25N01KV 1 Gbit SPI NAND on SPI2 — raw page/block driver (no filesystem).
 * 2048 B pages + 64 B spare, 64 pages/block, 1024 blocks, on-die ECC (1 bit/512 B), BUF=1 mode.
 * Verified 2026-10-21: 0 factory bad blocks on this unit; 40 MHz fast-read clean.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NAND_PAGE_SIZE     2048
#define NAND_SPARE_SIZE    64
#define NAND_PAGES_PER_BLK 64
#define NAND_BLOCKS        1024
#define NAND_BLOCK_SIZE    (NAND_PAGE_SIZE * NAND_PAGES_PER_BLK)   /* 128 KB */
#define NAND_PAGES         (NAND_BLOCKS * NAND_PAGES_PER_BLK)

typedef struct {
    uint8_t  jedec[3];          /* EF AE 21 */
    uint8_t  sr1, sr2;          /* protection, configuration */
    int      bad_blocks;        /* from the factory-marker scan */
    uint32_t clock_hz;
} nand_info_t;

/* Add device (clock_hz: 20 MHz safe everywhere; 40 MHz verified on rev 20), reset, read ID,
 * clear block protection, scan factory bad-block markers. */
esp_err_t nand_init(uint32_t clock_hz);
const nand_info_t *nand_info(void);
bool      nand_block_is_bad(uint16_t blk);
void      nand_mark_bad(uint16_t blk);        /* runtime: remember (RAM) + write the spare marker */

/* Page address = blk * 64 + page. All calls block until the chip is ready (≤ ~10 ms erase). */
esp_err_t nand_block_erase(uint16_t blk);                                   /* E-FAIL -> ESP_FAIL */
esp_err_t nand_page_program(uint32_t page, const uint8_t *data, size_t len); /* len ≤ 2048, from column 0; P-FAIL -> ESP_FAIL */
/* Read `len` bytes from `col` of `page` into dst. ecc_status (may be NULL): 0 ok, 1 corrected, 2 uncorrectable. */
esp_err_t nand_page_read(uint32_t page, uint16_t col, uint8_t *dst, size_t len, int *ecc_status);
/* Read the spare area (64 B at column 2048). */
esp_err_t nand_spare_read(uint32_t page, uint8_t *dst64);

/* Low-level (selftest) */
uint8_t   nand_status(uint8_t addr);          /* 0xA0 / 0xB0 / 0xC0 */
