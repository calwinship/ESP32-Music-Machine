// INDEX.BIN parser (plain C, no ESP-IDF headers; host-tested against tests/host/fixtures/index_v1.bin).
// The format is written by tools/sync_tunes.py (write_index): little-endian, CRC32 at the end. tune_index_open()
// checks every byte the getters will read, so a corrupt or truncated file is rejected up front.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUNE_INDEX_VERSION     1
#define TUNE_INDEX_MAX_MARKERS 8
#define TUNE_INDEX_MAX_TITLE   100   // bytes of UTF-8
#define TUNE_INDEX_MAX_NAME    12    // 8.3 file name

typedef enum {
    TUNE_INDEX_OK = 0,
    TUNE_INDEX_ERR_SIZE,     // too short, or length field doesn't match
    TUNE_INDEX_ERR_CRC,
    TUNE_INDEX_ERR_HEADER,   // magic, version or codec
    TUNE_INDEX_ERR_RECORD,   // a tune record is inconsistent or runs past the end
} tune_index_err_t;

typedef struct {
    const uint8_t *data;     // caller keeps the buffer alive
    size_t len;
    uint16_t count;
} tune_index_t;

typedef struct {
    uint32_t tune_id;            // stable: FNV-1a of the normalised title (play history key)
    uint32_t sample_rate;
    uint32_t total_samples;      // playable samples (tune timeline)
    uint32_t file_bytes;
    uint16_t delay;              // decoded samples before tune sample 0
    uint16_t samples_per_frame;
    uint16_t seconds_count;      // = ceil(total_samples / sample_rate)
    uint8_t channels;
    uint8_t marker_count;
    char file_name[TUNE_INDEX_MAX_NAME + 1];
    char title[TUNE_INDEX_MAX_TITLE + 1];
    uint32_t markers_ms[TUNE_INDEX_MAX_MARKERS];  // start of each play, increasing
    const uint8_t *seconds;      // seconds_count x {u32 byte offset, u32 frame}; read with tune_second()
} tune_info_t;

uint32_t tune_crc32(const uint8_t *p, size_t n);   // IEEE 802.3 (same as Python's zlib.crc32)

tune_index_err_t tune_index_open(tune_index_t *ix, const uint8_t *data, size_t len);
const char *tune_index_err_str(tune_index_err_t e);

// Tune i (0-based, in title order). Returns false if i >= count.
bool tune_index_get(const tune_index_t *ix, uint16_t i, tune_info_t *out);

// Where decoding must start for any target sample inside second s (s < seconds_count).
void tune_second(const tune_info_t *t, uint32_t s, uint32_t *byte_offset, uint32_t *start_frame);

#ifdef __cplusplus
}
#endif
