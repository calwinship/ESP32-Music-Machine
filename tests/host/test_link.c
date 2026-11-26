// Host tests for logic/link (USB link framing between ./sync-tunes and the board).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "logic/link.h"

static int g_checks, g_failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        g_checks++;                                                              \
        if (_a != _b) {                                                          \
            g_failures++;                                                        \
            printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, \
                   #a, _a, _b);                                                  \
        }                                                                        \
    } while (0)

// 'i' {1, 0, 2, 983040, 16}, made with Python's zlib (same bytes in test_sync_tunes.py)
static const uint8_t GOLDEN[] = {0x4b, 0x4c, 0x69, 0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x10, 0x00, 0x00,
                                 0x00, 0xe2, 0x79, 0x32, 0x55};

static uint8_t g_buf[LINK_OVERHEAD + LINK_MAX_PAYLOAD];

static void test_crc(void)
{
    CHECK_EQ(link_crc32(0, "123456789", 9), 0xCBF43926u);   // the CRC-32 check value
    CHECK_EQ(link_crc32(link_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
    CHECK_EQ(link_crc32(0, "", 0), 0);
}

static void test_encode_golden(void)
{
    uint8_t p[20], out[64];
    uint32_t v[5] = {1, 0, 2, 983040, 16};
    for (int i = 0; i < 5; i++) link_put_u32(p + 4 * i, v[i]);
    CHECK_EQ(link_encode('i', p, sizeof p, out, sizeof out), sizeof GOLDEN);
    CHECK(memcmp(out, GOLDEN, sizeof GOLDEN) == 0);
    CHECK_EQ(link_encode('i', p, sizeof p, out, sizeof GOLDEN - 1), 0);     // does not fit
    CHECK_EQ(link_encode('W', g_buf, LINK_MAX_PAYLOAD + 1, g_buf, sizeof g_buf), 0);
}

// Feeds `data` in pieces of `step` bytes; returns frames found (types into types[]).
static int feed_all(link_rx_t *rx, const uint8_t *data, size_t n, size_t step, uint8_t *types, uint32_t *lens)
{
    int found = 0;
    size_t off = 0;
    while (off < n) {
        size_t piece = (n - off < step) ? n - off : step;
        size_t done = 0;
        while (done < piece) {
            size_t used;
            link_frame_t f;
            if (link_rx_feed(rx, data + off + done, piece - done, &used, &f)) {
                types[found] = f.type;
                lens[found++] = f.len;
            }
            done += used;
        }
        off += piece;
    }
    return found;
}

static void test_decode_between_log_text(void)
{
    static uint8_t stream[4096];
    size_t n = 0;
    const char *log1 = "I (786) app: volume 8; KLK resume point found\r\n";
    memcpy(stream + n, log1, strlen(log1)); n += strlen(log1);
    memcpy(stream + n, GOLDEN, sizeof GOLDEN); n += sizeof GOLDEN;
    memcpy(stream + n, "KL", 2); n += 2;                         // a broken start, then a real frame
    uint8_t big[LINK_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);
    static uint8_t frame2[LINK_OVERHEAD + LINK_MAX_PAYLOAD];
    size_t f2 = link_encode('W', big, 1000, frame2, sizeof frame2);
    memcpy(stream + n, frame2, f2); n += f2;
    const char *log2 = "W (900) player: KL";
    memcpy(stream + n, log2, strlen(log2)); n += strlen(log2);
    memcpy(stream + n, GOLDEN, sizeof GOLDEN); n += sizeof GOLDEN;

    static const size_t steps[] = {1, 3, 7, 64, 4096};
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        link_rx_t rx;
        link_rx_init(&rx, g_buf, sizeof g_buf);
        uint8_t types[8];
        uint32_t lens[8];
        CHECK_EQ(feed_all(&rx, stream, n, steps[s], types, lens), 3);
        CHECK_EQ(types[0], 'i'); CHECK_EQ(lens[0], 20);
        CHECK_EQ(types[1], 'W'); CHECK_EQ(lens[1], 1000);
        CHECK_EQ(types[2], 'i');
        CHECK_EQ(rx.bad_crc, 0);
    }
}

static void test_payload_is_returned(void)
{
    link_rx_t rx;
    link_rx_init(&rx, g_buf, sizeof g_buf);
    size_t used;
    link_frame_t f;
    CHECK(link_rx_feed(&rx, GOLDEN, sizeof GOLDEN, &used, &f));
    CHECK_EQ(used, sizeof GOLDEN);
    CHECK_EQ(link_get_u32(f.payload + 12), 983040);
    CHECK_EQ(link_get_u32(f.payload + 16), 16);
}

static void test_bad_crc_and_oversize_are_dropped(void)
{
    uint8_t bad[sizeof GOLDEN];
    memcpy(bad, GOLDEN, sizeof bad);
    bad[10] ^= 1;
    link_rx_t rx;
    link_rx_init(&rx, g_buf, sizeof g_buf);
    uint8_t types[4];
    uint32_t lens[4];
    CHECK_EQ(feed_all(&rx, bad, sizeof bad, 5, types, lens), 0);
    CHECK_EQ(rx.bad_crc, 1);
    CHECK_EQ(feed_all(&rx, GOLDEN, sizeof GOLDEN, 5, types, lens), 1);   // the next good frame still decodes

    uint8_t huge[LINK_HEADER] = {'K', 'L', 'W', 0xff, 0xff, 0xff, 0x7f};
    link_rx_init(&rx, g_buf, sizeof g_buf);
    CHECK_EQ(feed_all(&rx, huge, sizeof huge, 1, types, lens), 0);
    CHECK_EQ(feed_all(&rx, GOLDEN, sizeof GOLDEN, 1, types, lens), 1);

    uint8_t small_buf[64];                                         // a frame bigger than the buffer is skipped
    link_rx_init(&rx, small_buf, sizeof small_buf);
    static uint8_t frame[LINK_OVERHEAD + 100], zeros[100];
    size_t fl = link_encode('W', zeros, 100, frame, sizeof frame);
    CHECK_EQ(feed_all(&rx, frame, fl, 9, types, lens), 0);
    CHECK_EQ(feed_all(&rx, GOLDEN, sizeof GOLDEN, 9, types, lens), 1);
}

int main(void)
{
    test_crc();
    test_encode_golden();
    test_decode_between_log_text();
    test_payload_is_returned();
    test_bad_crc_and_oversize_are_dropped();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
