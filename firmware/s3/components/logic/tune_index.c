#include "logic/tune_index.h"

#include <string.h>

#define HEADER_LEN 16
#define RECORD_LEN 36
#define CODEC_MP3  1

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t tune_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

const char *tune_index_err_str(tune_index_err_t e)
{
    switch (e) {
    case TUNE_INDEX_OK: return "ok";
    case TUNE_INDEX_ERR_SIZE: return "bad size";
    case TUNE_INDEX_ERR_CRC: return "bad CRC";
    case TUNE_INDEX_ERR_HEADER: return "bad header (magic/version/codec)";
    case TUNE_INDEX_ERR_RECORD: return "bad tune record";
    }
    return "?";
}

// Parse and fully validate the record at data[pos], which must end by `end`. On success fills *t and *next.
static bool parse_record(const uint8_t *data, size_t pos, size_t end, tune_info_t *t, size_t *next)
{
    if (pos > end || end - pos < RECORD_LEN) {
        return false;
    }
    const uint8_t *r = data + pos;
    uint32_t rec_len = rd32(r);
    memset(t, 0, sizeof(*t));
    t->tune_id = rd32(r + 4);
    t->sample_rate = rd32(r + 8);
    t->total_samples = rd32(r + 12);
    t->file_bytes = rd32(r + 16);
    t->delay = (uint16_t)rd16(r + 20);
    t->samples_per_frame = (uint16_t)rd16(r + 22);
    t->seconds_count = (uint16_t)rd16(r + 24);
    t->channels = r[26];
    t->marker_count = r[27];
    uint32_t name_len = r[28], title_len = r[29];
    t->icon = (uint16_t)rd16(r + 30);
    t->colour = rd32(r + 32);

    if (rec_len < RECORD_LEN || rec_len > end - pos) {
        return false;
    }
    uint64_t expect = (uint64_t)RECORD_LEN + name_len + title_len + 4u * t->marker_count + 8u * t->seconds_count;
    if (expect != rec_len) {
        return false;
    }
    if (t->tune_id == 0 || t->sample_rate < 8000 || t->sample_rate > 192000 || t->samples_per_frame == 0 ||
        t->channels < 1 || t->channels > 2 || t->total_samples == 0 || t->file_bytes == 0 ||
        t->marker_count > TUNE_INDEX_MAX_MARKERS || name_len < 1 || name_len > TUNE_INDEX_MAX_NAME ||
        title_len < 1 || title_len > TUNE_INDEX_MAX_TITLE || t->icon < 0xE000 || t->icon > 0xF8FF ||
        t->colour > 0xFFFFFFu) {
        return false;
    }
    if (t->seconds_count != (t->total_samples + (uint64_t)t->sample_rate - 1) / t->sample_rate) {
        return false;
    }

    const uint8_t *p = r + RECORD_LEN;
    for (uint32_t i = 0; i < name_len; i++) {
        if (p[i] < 0x21 || p[i] > 0x7E || p[i] == '/') {
            return false;
        }
    }
    memcpy(t->file_name, p, name_len);
    p += name_len;
    for (uint32_t i = 0; i < title_len; i++) {
        if (p[i] == 0) {
            return false;
        }
    }
    memcpy(t->title, p, title_len);
    p += title_len;

    uint64_t duration_ms = (uint64_t)t->total_samples * 1000u / t->sample_rate;
    for (uint32_t i = 0; i < t->marker_count; i++) {
        t->markers_ms[i] = rd32(p + 4 * i);
        if (t->markers_ms[i] >= duration_ms || (i && t->markers_ms[i] <= t->markers_ms[i - 1])) {
            return false;
        }
    }
    p += 4u * t->marker_count;

    t->seconds = p;
    uint32_t prev_off = 0, prev_frame = 0;
    for (uint32_t s = 0; s < t->seconds_count; s++) {
        uint32_t off = rd32(p + 8 * s), frame = rd32(p + 8 * s + 4);
        // The start frame must come no later than the first frame of that second, and both must be monotonic.
        uint64_t first_frame = ((uint64_t)s * t->sample_rate + t->delay) / t->samples_per_frame;
        if (off >= t->file_bytes || frame > first_frame || off < prev_off || frame < prev_frame) {
            return false;
        }
        prev_off = off;
        prev_frame = frame;
    }
    *next = pos + rec_len;
    return true;
}

tune_index_err_t tune_index_open(tune_index_t *ix, const uint8_t *data, size_t len)
{
    memset(ix, 0, sizeof(*ix));
    if (!data || len < HEADER_LEN + 4) {
        return TUNE_INDEX_ERR_SIZE;
    }
    if (tune_crc32(data, len - 4) != rd32(data + len - 4)) {
        return TUNE_INDEX_ERR_CRC;
    }
    if (memcmp(data, "TIDX", 4) != 0 || rd16(data + 4) != TUNE_INDEX_VERSION || data[8] != CODEC_MP3) {
        return TUNE_INDEX_ERR_HEADER;
    }
    if (rd32(data + 12) != len) {
        return TUNE_INDEX_ERR_SIZE;
    }
    uint16_t count = (uint16_t)rd16(data + 6);
    size_t pos = HEADER_LEN, end = len - 4;
    for (uint16_t i = 0; i < count; i++) {
        tune_info_t t;
        if (!parse_record(data, pos, end, &t, &pos)) {
            return TUNE_INDEX_ERR_RECORD;
        }
    }
    if (pos != end) {
        return TUNE_INDEX_ERR_RECORD;
    }
    ix->data = data;
    ix->len = len;
    ix->count = count;
    return TUNE_INDEX_OK;
}

bool tune_index_get(const tune_index_t *ix, uint16_t i, tune_info_t *out)
{
    if (!ix->data || i >= ix->count) {
        return false;
    }
    size_t pos = HEADER_LEN, end = ix->len - 4;
    for (uint16_t k = 0; k <= i; k++) {
        if (!parse_record(ix->data, pos, end, out, &pos)) {
            return false;  // cannot happen after a successful tune_index_open
        }
    }
    return true;
}

void tune_second(const tune_info_t *t, uint32_t s, uint32_t *byte_offset, uint32_t *start_frame)
{
    if (s >= t->seconds_count) {
        s = t->seconds_count - 1u;
    }
    *byte_offset = rd32(t->seconds + 8u * s);
    *start_frame = rd32(t->seconds + 8u * s + 4u);
}
