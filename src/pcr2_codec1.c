// PCR2 codec 1 encoder (data-v2 §1.4) -- byte-for-byte identical to make_pcr2.encode_c1 (tested).
// Per field, in header order: the first value (field's own size, little-endian), then the n-1 deltas in frames
// of 32: u8 width w, then the frame's zigzag deltas packed LSB-first at w bits each, padded to a whole byte.
// Deltas wrap at the field's width (so w <= 8 * size). No allocation, no library, integer ops only.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline uint32_t rd_le(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

// Worst case output: n*sample_size + 2*n_fields*ceil(n/32) bytes (each frame: 1 width byte + at most 1 padding byte).
size_t pcr2_codec1_encode(const uint8_t *samples, uint16_t n, uint16_t sample_size,
                          const uint8_t *field_size, const uint16_t *field_off, int n_fields, uint8_t *out)
{
    uint8_t *o = out;
    if (n == 0) return 0;
    for (int f = 0; f < n_fields; f++) {
        const int sz = field_size[f], bits = 8 * sz;
        const uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
        const uint8_t *p = samples + field_off[f];
        memcpy(o, p, sz);                              // first value, as stored (little-endian)
        o += sz;
        uint32_t prev = rd_le(p, sz);
        for (int a = 1; a < n; a += 32) {
            const int cnt = (n - a) < 32 ? (n - a) : 32;
            uint32_t zz[32], all = 0;
            for (int k = 0; k < cnt; k++) {
                const uint32_t cur = rd_le(p + (size_t)(a + k) * sample_size, sz);
                const uint32_t d = (cur - prev) & mask;                 // wraps at the field width
                const uint32_t neg = (d >> (bits - 1)) & 1;
                zz[k] = ((d << 1) ^ (neg ? mask : 0)) & mask;           // zigzag
                all |= zz[k];
                prev = cur;
            }
            const int w = all ? 32 - __builtin_clz(all) : 0;
            *o++ = (uint8_t)w;
            uint64_t acc = 0;
            int nb = 0;
            for (int k = 0; k < cnt; k++) {
                acc |= (uint64_t)zz[k] << nb;
                nb += w;
                while (nb >= 8) { *o++ = (uint8_t)acc; acc >>= 8; nb -= 8; }
            }
            if (nb) *o++ = (uint8_t)acc;
        }
    }
    return (size_t)(o - out);
}
