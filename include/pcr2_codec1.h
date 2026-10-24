/* PCR2 codec 1 — delta bit-packing (docs/data-v2.md §1.4). Reference encoder from the backend, byte-identical to
 * make_pcr2.encode_c1. Portable C, no allocation. Worst-case output: n*sample_size + 2*n_fields*ceil(n/32). */
#pragma once
#include <stdint.h>
#include <stddef.h>
size_t pcr2_codec1_encode(const uint8_t *samples, uint16_t n, uint16_t sample_size,
                          const uint8_t *field_size, const uint16_t *field_off, int n_fields, uint8_t *out);
