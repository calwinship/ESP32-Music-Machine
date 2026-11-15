// Host tests for logic/tune_index and logic/playback (built and run by ./test from the repo root).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logic/playback.h"
#include "logic/tune_index.h"

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
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        g_checks++;                                                              \
        if (strcmp((a), (b)) != 0) {                                             \
            g_failures++;                                                        \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                        \
    } while (0)

#define FIXTURE "tests/host/fixtures/index_v2.bin"

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s (run from the repo root)\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void reseal(uint8_t *buf, size_t len)
{
    put32(buf + len - 4, tune_crc32(buf, len - 4));
}

// --- CRC + index -----------------------------------------------------------------------------------------------
static void test_crc32_reference(void)
{
    CHECK_EQ(tune_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ(tune_crc32((const uint8_t *)"", 0), 0);
}

static void test_fixture_from_python_writer(void)
{
    size_t len;
    uint8_t *buf = load(FIXTURE, &len);
    tune_index_t ix;
    CHECK_EQ(tune_index_open(&ix, buf, len), TUNE_INDEX_OK);
    CHECK_EQ(ix.count, 2);

    tune_info_t t;
    CHECK(tune_index_get(&ix, 0, &t));
    CHECK_EQ(t.tune_id, 0x1a7c2172u);  // FNV-1a("clare jig"): same ID sync-tunes printed for the real tune
    CHECK_STR(t.title, "Clare Jig");
    CHECK_STR(t.file_name, "T01.MP3");
    CHECK_EQ(t.sample_rate, 32000);
    CHECK_EQ(t.channels, 1);
    CHECK_EQ(t.total_samples, 96000);
    CHECK_EQ(t.file_bytes, 864);
    CHECK_EQ(t.delay, 1105);
    CHECK_EQ(t.samples_per_frame, 1152);
    CHECK_EQ(t.seconds_count, 3);
    CHECK_EQ(t.marker_count, 2);
    CHECK_EQ(t.icon, 0xF06C);          // leaf
    CHECK_EQ(t.colour, 0x2EC4B6);
    CHECK_EQ(t.markers_ms[0], 500);
    CHECK_EQ(t.markers_ms[1], 2000);
    uint32_t off, frame;
    tune_second(&t, 2, &off, &frame);
    CHECK_EQ(off, 288);
    CHECK_EQ(frame, 2);
    tune_second(&t, 99, &off, &frame);  // clamps to the last second
    CHECK_EQ(off, 288);

    CHECK(tune_index_get(&ix, 1, &t));
    CHECK_STR(t.title, "N\xc3\xb3ra Chr\xc3\xadonna");
    CHECK_EQ(t.marker_count, 0);
    CHECK_EQ(t.icon, 0xF186);          // moon
    CHECK(!tune_index_get(&ix, 2, &t));
    free(buf);
}

static void test_every_truncation_is_rejected(void)
{
    size_t len;
    uint8_t *buf = load(FIXTURE, &len);
    tune_index_t ix;
    for (size_t n = 0; n < len; n++) {
        uint8_t *copy = malloc(n ? n : 1);  // exact-size copy so ASan catches any read past the end
        memcpy(copy, buf, n);
        CHECK(tune_index_open(&ix, copy, n) != TUNE_INDEX_OK);
        free(copy);
    }
    free(buf);
}

static void test_every_bit_flip_is_rejected(void)
{
    size_t len;
    uint8_t *buf = load(FIXTURE, &len);
    tune_index_t ix;
    int accepted = 0;
    for (size_t i = 0; i < len * 8; i++) {
        buf[i / 8] ^= (uint8_t)(1u << (i % 8));
        accepted += tune_index_open(&ix, buf, len) == TUNE_INDEX_OK;
        buf[i / 8] ^= (uint8_t)(1u << (i % 8));
    }
    CHECK_EQ(accepted, 0);
    free(buf);
}

static void test_consistent_but_wrong_records_are_rejected(void)
{
    // Edits with a recomputed CRC: the structure checks must catch them on their own.
    size_t len;
    uint8_t *orig = load(FIXTURE, &len);
    uint8_t *buf = malloc(len);
    tune_index_t ix;
    const size_t rec = 16;  // first record starts after the 16-byte header
    struct { size_t at; uint32_t value; int width; const char *what; } cases[] = {
        {rec + 4, 0, 4, "tune id 0"},
        {rec + 8, 4000, 4, "sample rate too low"},
        {rec + 24, 4, 2, "seconds_count != ceil(total / rate)"},
        {rec + 26, 3, 1, "3 channels"},
        {rec + 27, 9, 1, "too many markers"},
        {rec + 36 + 1, '/', 1, "slash in file name"},
        {rec + 36 + 7 + 9, 3000, 4, "marker past the end"},
        {rec + 36 + 7 + 9 + 4, 400, 4, "markers not increasing"},
        {rec + 36 + 7 + 9 + 8 + 8, 900, 4, "second offset past the file"},
        {rec + 36 + 7 + 9 + 8 + 20, 0, 4, "start frames not monotonic"},
        {rec + 30, 0x0041, 2, "icon not a private-use code point"},
        {rec + 32, 0x01000000, 4, "colour above 0xFFFFFF"},
        {12, 1, 4, "length field"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        memcpy(buf, orig, len);
        if (cases[c].width == 4) {
            put32(buf + cases[c].at, cases[c].value);
        } else if (cases[c].width == 2) {
            buf[cases[c].at] = (uint8_t)cases[c].value;
            buf[cases[c].at + 1] = (uint8_t)(cases[c].value >> 8);
        } else {
            buf[cases[c].at] = (uint8_t)cases[c].value;
        }
        reseal(buf, len);
        if (tune_index_open(&ix, buf, len) == TUNE_INDEX_OK) {
            g_failures++;
            printf("FAIL accepted: %s\n", cases[c].what);
        }
        g_checks++;
    }
    memcpy(buf, orig, len);
    CHECK_EQ(tune_index_open(&ix, buf, len), TUNE_INDEX_OK);  // the unedited copy still passes
    free(buf);
    free(orig);
}

// --- seek + gate -----------------------------------------------------------------------------------------------
static tune_info_t fixture_tune(uint8_t **keep)
{
    size_t len;
    *keep = load(FIXTURE, &len);
    tune_index_t ix;
    tune_info_t t;
    tune_index_open(&ix, *keep, len);
    tune_index_get(&ix, 0, &t);
    return t;
}

static void test_seek_plan_maths(void)
{
    uint8_t *keep;
    tune_info_t t = fixture_tune(&keep);
    seek_plan_t p;
    CHECK(seek_plan(&t, 0, &p));
    CHECK_EQ(p.target_frame, 0);           // (0 + 1105) / 1152
    CHECK_EQ(p.skip_in_target, 1105);
    CHECK_EQ(p.start_frame, 0);
    CHECK(seek_plan(&t, 40000, &p));       // second 1 -> index entry (144, 1)
    CHECK_EQ(p.byte_offset, 144);
    CHECK_EQ(p.start_frame, 1);
    CHECK_EQ(p.target_frame, (40000 + 1105) / 1152);
    CHECK_EQ(p.skip_in_target, (40000 + 1105) % 1152);
    CHECK(seek_plan(&t, 95999, &p));
    CHECK(!seek_plan(&t, 96000, &p));
    CHECK_EQ(seek_ms_to_sample(6800, 32000), 217600);
    CHECK_EQ(seek_ms_to_sample(1, 32000), 32);
    free(keep);
}

static void test_gate_drops_preroll_and_skips_into_target(void)
{
    seek_plan_t p = {.start_frame = 10, .target_frame = 13, .skip_in_target = 100};
    frame_gate_t g;
    uint32_t first;
    frame_gate_init(&g, &p);
    CHECK(frame_gate_in_preroll(&g));
    CHECK_EQ(frame_gate_next(&g, 1152, &first), 0);   // frame 10
    CHECK_EQ(frame_gate_next(&g, 0, &first), 0);      // frame 11 failed to decode: still counted
    CHECK_EQ(frame_gate_next(&g, 1152, &first), 0);   // frame 12
    CHECK(!frame_gate_in_preroll(&g));
    CHECK_EQ(frame_gate_next(&g, 1152, &first), 1052);  // frame 13: target
    CHECK_EQ(first, 100);
    CHECK_EQ(frame_gate_next(&g, 1152, &first), 1152);
    CHECK_EQ(first, 0);
    p.skip_in_target = 2000;                           // more than the frame produced: keep none, never underflow
    frame_gate_init(&g, &p);
    g.frame = 13;
    CHECK_EQ(frame_gate_next(&g, 1152, &first), 0);
}

// Simulate a decoder on a synthetic timeline: decoded sample n has value n. Whatever the seek target, the first
// kept sample must be target + delay.
static void test_gate_and_plan_reach_exact_sample(void)
{
    uint8_t *keep;
    tune_info_t t = fixture_tune(&keep);
    uint32_t targets[] = {0, 1, 46, 1151, 32000, 40000, 63999, 64000, 95999};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        seek_plan_t p;
        CHECK(seek_plan(&t, targets[i], &p));
        frame_gate_t g;
        frame_gate_init(&g, &p);
        uint32_t got = UINT32_MAX;
        for (uint32_t f = p.start_frame; f <= p.target_frame + 1 && got == UINT32_MAX; f++) {
            uint32_t first, n = frame_gate_next(&g, t.samples_per_frame, &first);
            if (n) {
                got = f * t.samples_per_frame + first - t.delay;
            }
        }
        CHECK_EQ(got, targets[i]);
    }
    free(keep);
}

static void test_mp3_frame_len(void)
{
    const uint8_t mono32k32[4] = {0xFF, 0xFB, 0x18, 0xC4};      // as sync-tunes writes: 144 bytes
    const uint8_t info40k[4] = {0xFF, 0xFB, 0x28, 0xC0};        // 40 kbps at 32 kHz: 180 bytes
    const uint8_t padded44[4] = {0xFF, 0xFB, 0x12, 0xC0};       // 32 kbps 44.1 kHz + padding: 104 + 1
    const uint8_t crc48k48[4] = {0xFF, 0xFA, 0x34, 0xC0};       // 48 kbps 48 kHz with CRC: 144
    const uint8_t mpeg2[4] = {0xFF, 0xF3, 0x18, 0xC0};
    const uint8_t layer2[4] = {0xFF, 0xFD, 0x18, 0xC0};
    const uint8_t bad_rate[4] = {0xFF, 0xFB, 0x1C, 0xC0};
    const uint8_t free_br[4] = {0xFF, 0xFB, 0x08, 0xC0};
    const uint8_t id3[4] = {'I', 'D', '3', 4};
    CHECK_EQ(mp3_frame_len(mono32k32), 144);
    CHECK_EQ(mp3_frame_len(info40k), 180);
    CHECK_EQ(mp3_frame_len(padded44), 105);
    CHECK_EQ(mp3_frame_len(crc48k48), 144);
    CHECK_EQ(mp3_frame_len(mpeg2), 0);
    CHECK_EQ(mp3_frame_len(layer2), 0);
    CHECK_EQ(mp3_frame_len(bad_rate), 0);
    CHECK_EQ(mp3_frame_len(free_br), 0);
    CHECK_EQ(mp3_frame_len(id3), 0);
}

// --- player state ----------------------------------------------------------------------------------------------
static void test_player_play_pause_end(void)
{
    player_t p;
    player_init(&p);
    CHECK_EQ(p.state, PLAYER_IDLE);
    CHECK(!player_toggle(&p));
    CHECK_EQ(player_accept(&p, 100), 0);

    player_start(&p, 32000, 100000, 217600);   // starts past the end: ended at once
    CHECK_EQ(p.state, PLAYER_ENDED);

    player_start(&p, 32000, 100000, 32000);
    CHECK_EQ(p.state, PLAYER_PLAYING);
    CHECK_EQ(player_elapsed_ms(&p), 1000);
    CHECK(player_toggle(&p));
    CHECK_EQ(p.state, PLAYER_PAUSED);
    CHECK_EQ(player_accept(&p, 1152), 0);       // paused: nothing goes out, position frozen
    CHECK_EQ(p.position, 32000);
    CHECK(player_toggle(&p));
    CHECK_EQ(player_accept(&p, 1152), 1152);
    CHECK_EQ(p.position, 33152);
    while (p.state == PLAYER_PLAYING) {
        player_accept(&p, 1152);
    }
    CHECK_EQ(p.state, PLAYER_ENDED);
    CHECK_EQ(p.position, 100000);               // last frame trimmed to the tune's end (encoder padding dropped)
    CHECK(!player_toggle(&p));
    player_fail(&p);
    CHECK_EQ(p.state, PLAYER_ERROR);
    CHECK_STR(player_state_name(PLAYER_PAUSED), "paused");
}

static void test_player_logs_once_per_second(void)
{
    player_t p;
    player_init(&p);
    CHECK(!player_log_due(&p));
    player_start(&p, 32000, 32000 * 10, 6800 * 32);   // starts at 6.8 s
    int logs = 0;
    logs += player_log_due(&p);                        // at start
    CHECK_EQ(logs, 1);
    for (int i = 0; i < 300; i++) {                    // 300 x 1152 samples = 10.8 s, capped at the 10 s end
        player_accept(&p, 1152);
        logs += player_log_due(&p);
    }
    CHECK_EQ(logs, 1 + 4);                             // seconds 7, 8, 9 and the end (10)
}

static void test_player_counts_new_underruns(void)
{
    player_t p;
    player_init(&p);
    CHECK_EQ(player_new_underruns(&p, 0), 0);
    CHECK_EQ(player_new_underruns(&p, 3), 3);
    CHECK_EQ(player_new_underruns(&p, 3), 0);
    player_start(&p, 32000, 1000, 0);                  // a new tune keeps the running count
    CHECK_EQ(player_new_underruns(&p, 4), 1);
    p.underruns_seen = UINT32_MAX;
    CHECK_EQ(player_new_underruns(&p, 1), 2);          // counter wrap
}

// --- text ------------------------------------------------------------------------------------------------------
static void test_text(void)
{
    char b[64];
    text_mmss(b, sizeof(b), 0);
    CHECK_STR(b, "0:00");
    text_mmss(b, sizeof(b), 186999);
    CHECK_STR(b, "3:06");
    text_mmss(b, sizeof(b), 3723000);
    CHECK_STR(b, "1:02:03");
    text_mmss(b, 3, 186999);
    CHECK_STR(b, "3:");

    text_fold_ascii(b, sizeof(b), "N\xc3\xb3ra Chr\xc3\xadonna");
    CHECK_STR(b, "Nora Chrionna");
    text_fold_ascii(b, sizeof(b), "\xc3\x81\xc3\x89\xc3\x8d\xc3\x93\xc3\x9a \xc3\xa1\xc3\xa9\xc3\xad\xc3\xb3\xc3\xba");
    CHECK_STR(b, "AEIOU aeiou");
    text_fold_ascii(b, sizeof(b), "Johnny's \xe2\x80\x94 x");     // em dash (3 bytes) -> one '?'
    CHECK_STR(b, "Johnny's ? x");
    text_fold_ascii(b, sizeof(b), "a\xc3");                      // truncated sequence at the end
    CHECK_STR(b, "a?");
    text_fold_ascii(b, sizeof(b), "\xff\xfe" "b");
    CHECK_STR(b, "??b");
    text_fold_ascii(b, 4, "Clare Jig");
    CHECK_STR(b, "Cla");

    char u[5];
    CHECK_EQ(text_utf8(0xF06C, u), 3);
    CHECK_STR(u, "\xef\x81\xac");      // U+F06C (leaf) as LVGL's LV_SYMBOL_* strings are written
    CHECK_EQ(text_utf8('A', u), 1);
    CHECK_STR(u, "A");
    CHECK_EQ(text_utf8(0xF3, u), 2);
    CHECK_STR(u, "\xc3\xb3");          // o with fada
    CHECK_EQ(text_utf8(0x1F600, u), 4);
    CHECK_EQ(text_utf8(0xD800, u), 0);    // surrogate
    CHECK_EQ(text_utf8(0x110000, u), 0);
}

int main(void)
{
    test_crc32_reference();
    test_fixture_from_python_writer();
    test_every_truncation_is_rejected();
    test_every_bit_flip_is_rejected();
    test_consistent_but_wrong_records_are_rejected();
    test_seek_plan_maths();
    test_gate_drops_preroll_and_skips_into_target();
    test_gate_and_plan_reach_exact_sample();
    test_mp3_frame_len();
    test_player_play_pause_end();
    test_player_logs_once_per_second();
    test_player_counts_new_underruns();
    test_text();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
