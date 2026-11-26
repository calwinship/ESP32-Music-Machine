// USB link between ./sync-tunes (tools/board_link.py) and the board: framing and CRC. Plain C, host-tested
// (tests/host/test_link.c; the golden frame there is also checked by tests/host/test_sync_tunes.py).
//
// The link shares the USB-Serial-JTAG port with the log console, so the board's output is log text with frames mixed
// in. A frame is found by its magic and accepted only if its CRC matches, so any text around it is skipped.
//
//   frame = 'K' 'L' type(u8) len(u32 LE) payload[len] crc(u32 LE)     crc = CRC-32 (zlib) over type, len, payload
//
// Messages (u32 / i32 little-endian):
//   host -> board            board -> host
//   'I' info                 'i' {version, mode, source, card_sectors, tune_count}
//   'B' begin sync           'b' {}           then the board restarts into sync mode
//   'S' start {total}        's' {err}        sync mode: clears sector 0 (card invalid until the end)
//   'W' write {first, data}  'w' {first, count, err}   data = count x 512 bytes; board writes, reads back, compares
//   'X' exit                 'x' {}           then the board restarts as the player
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_VERSION       1
#define LINK_HEADER        7                     // magic(2) + type(1) + len(4)
#define LINK_OVERHEAD      (LINK_HEADER + 4)
#define LINK_SECTOR        512
#define LINK_MAX_SECTORS   64                    // per 'W' frame (32 KB)
#define LINK_MAX_PAYLOAD   (4 + LINK_MAX_SECTORS * LINK_SECTOR)

enum { LINK_MODE_PLAYER = 0, LINK_MODE_SYNC = 1 };
enum { LINK_SOURCE_NONE = 0, LINK_SOURCE_FLASH = 1, LINK_SOURCE_SD = 2 };

// CRC-32 as zlib.crc32: start with crc = 0, chain calls to continue.
uint32_t link_crc32(uint32_t crc, const void *data, size_t len);

// Writes a frame to out. Returns its length, or 0 if it does not fit in cap or len > LINK_MAX_PAYLOAD.
size_t link_encode(uint8_t type, const void *payload, uint32_t len, uint8_t *out, size_t cap);

typedef struct {
    uint8_t *buf;          // holds one frame, LINK_OVERHEAD + LINK_MAX_PAYLOAD bytes (or less: max_payload)
    size_t cap;
    size_t have;
    uint32_t skipped;      // bytes outside frames (log text, noise)
    uint32_t bad_crc;      // frames dropped for a CRC mismatch
} link_rx_t;

typedef struct {
    uint8_t type;
    uint32_t len;
    const uint8_t *payload;   // inside rx->buf; valid until the next link_rx_feed
} link_frame_t;

void link_rx_init(link_rx_t *rx, uint8_t *buf, size_t cap);

// Consumes bytes from data until a frame completes or data runs out; *used = bytes consumed. Returns true with *out
// filled when a frame is complete (feed the rest of data in the next call).
bool link_rx_feed(link_rx_t *rx, const uint8_t *data, size_t n, size_t *used, link_frame_t *out);

static inline uint32_t link_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void link_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#ifdef __cplusplus
}
#endif
