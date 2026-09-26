#pragma once
#include <stdint.h>

#define GBK_LEAD_MIN  0x81
#define GBK_LEAD_MAX  0xFE
#define GBK_TRAIL_CNT 190
#define GBK_MAP_SIZE  23940

extern const uint16_t gbk_unicode_map[GBK_MAP_SIZE];

static inline uint32_t gbk_lookup(uint8_t b1, uint8_t b2)
{
    if (b1 < GBK_LEAD_MIN || b1 > GBK_LEAD_MAX) return 0;
    if (b2 < 0x40 || b2 > 0xFE || b2 == 0x7F) return 0;

    const int ti = (b2 <= 0x7E) ? (b2 - 0x40) : (b2 - 0x80 + 63);
    return gbk_unicode_map[(b1 - GBK_LEAD_MIN) * GBK_TRAIL_CNT + ti];
}
