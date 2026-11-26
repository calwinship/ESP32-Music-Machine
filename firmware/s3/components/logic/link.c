#include "logic/link.h"

#include <string.h>

uint32_t link_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

size_t link_encode(uint8_t type, const void *payload, uint32_t len, uint8_t *out, size_t cap)
{
    if (len > LINK_MAX_PAYLOAD || cap < (size_t)len + LINK_OVERHEAD) {
        return 0;
    }
    out[0] = 'K';
    out[1] = 'L';
    out[2] = type;
    link_put_u32(out + 3, len);
    if (len) {
        memcpy(out + LINK_HEADER, payload, len);
    }
    link_put_u32(out + LINK_HEADER + len, link_crc32(0, out + 2, 5 + len));
    return LINK_OVERHEAD + len;
}

void link_rx_init(link_rx_t *rx, uint8_t *buf, size_t cap)
{
    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap;
}

// Header bytes so far are not a valid start: drop the first byte and re-scan the rest for the magic.
static void resync(link_rx_t *rx)
{
    size_t i = 1;
    while (i < rx->have && rx->buf[i] != 'K') {
        i++;
    }
    rx->skipped += (uint32_t)i;
    memmove(rx->buf, rx->buf + i, rx->have - i);
    rx->have -= i;
}

// True while the buffered bytes can still be the start of a frame.
static bool header_ok(const link_rx_t *rx)
{
    if (rx->have >= 1 && rx->buf[0] != 'K') return false;
    if (rx->have >= 2 && rx->buf[1] != 'L') return false;
    if (rx->have >= LINK_HEADER) {
        uint32_t len = link_get_u32(rx->buf + 3);
        if (len > LINK_MAX_PAYLOAD || (size_t)len + LINK_OVERHEAD > rx->cap) return false;
    }
    return true;
}

bool link_rx_feed(link_rx_t *rx, const uint8_t *data, size_t n, size_t *used, link_frame_t *out)
{
    size_t i = 0;
    while (i < n) {
        if (rx->have == 0 && data[i] != 'K') {   // fast skip of text between frames
            rx->skipped++;
            i++;
            continue;
        }
        size_t want = LINK_HEADER;
        if (rx->have >= LINK_HEADER) {
            want = LINK_OVERHEAD + link_get_u32(rx->buf + 3);
        }
        size_t take = want - rx->have;
        if (take > n - i) take = n - i;
        if (rx->have < LINK_HEADER && take > LINK_HEADER - rx->have) take = LINK_HEADER - rx->have;
        memcpy(rx->buf + rx->have, data + i, take);
        rx->have += take;
        i += take;
        while (rx->have && !header_ok(rx)) {
            resync(rx);
        }
        if (rx->have >= LINK_HEADER) {
            uint32_t len = link_get_u32(rx->buf + 3);
            if (rx->have == (size_t)len + LINK_OVERHEAD) {
                rx->have = 0;
                if (link_crc32(0, rx->buf + 2, 5 + len) != link_get_u32(rx->buf + LINK_HEADER + len)) {
                    rx->bad_crc++;
                    continue;
                }
                out->type = rx->buf[2];
                out->len = len;
                out->payload = rx->buf + LINK_HEADER;
                *used = i;
                return true;
            }
        }
    }
    *used = i;
    return false;
}
