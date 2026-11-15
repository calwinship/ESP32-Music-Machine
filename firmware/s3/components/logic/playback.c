#include "logic/playback.h"

#include <stdio.h>
#include <string.h>

// ---- seeking -------------------------------------------------------------------------------------------------
bool seek_plan(const tune_info_t *t, uint32_t target_sample, seek_plan_t *out)
{
    if (target_sample >= t->total_samples) {
        return false;
    }
    uint64_t decoded = (uint64_t)target_sample + t->delay;
    out->target_sample = target_sample;
    out->target_frame = (uint32_t)(decoded / t->samples_per_frame);
    out->skip_in_target = (uint32_t)(decoded % t->samples_per_frame);
    tune_second(t, target_sample / t->sample_rate, &out->byte_offset, &out->start_frame);
    return true;
}

uint32_t seek_ms_to_sample(uint32_t ms, uint32_t sample_rate)
{
    return (uint32_t)((uint64_t)ms * sample_rate / 1000u);
}

void frame_gate_init(frame_gate_t *g, const seek_plan_t *p)
{
    g->frame = p->start_frame;
    g->target_frame = p->target_frame;
    g->skip = p->skip_in_target;
}

uint32_t frame_gate_next(frame_gate_t *g, uint32_t decoded, uint32_t *first)
{
    uint32_t f = g->frame++;
    *first = 0;
    if (f < g->target_frame) {
        return 0;
    }
    if (f == g->target_frame) {
        uint32_t skip = g->skip < decoded ? g->skip : decoded;
        *first = skip;
        return decoded - skip;
    }
    return decoded;
}

bool frame_gate_in_preroll(const frame_gate_t *g)
{
    return g->frame < g->target_frame;
}

// ---- MP3 frames ----------------------------------------------------------------------------------------------
uint32_t mp3_frame_len(const uint8_t *p)
{
    static const uint16_t kbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint32_t rates[4] = {44100, 48000, 32000, 0};
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xFA) {   // sync, MPEG-1, Layer III (protection bit either way)
        return 0;
    }
    uint32_t br = kbps[p[2] >> 4], sr = rates[(p[2] >> 2) & 3], pad = (p[2] >> 1) & 1u;
    if (!br || !sr) {
        return 0;
    }
    return 144000u * br / sr + pad;
}

// ---- player state --------------------------------------------------------------------------------------------
void player_init(player_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = PLAYER_IDLE;
}

void player_start(player_t *p, uint32_t sample_rate, uint32_t total_samples, uint32_t start_sample)
{
    uint32_t underruns = p->underruns_seen;
    player_init(p);
    p->underruns_seen = underruns;
    p->sample_rate = sample_rate ? sample_rate : 1;
    p->total_samples = total_samples;
    p->position = start_sample < total_samples ? start_sample : total_samples;
    p->next_log_second = p->position / p->sample_rate;
    p->state = p->position < total_samples ? PLAYER_PLAYING : PLAYER_ENDED;
}

bool player_toggle(player_t *p)
{
    if (p->state == PLAYER_PLAYING) {
        p->state = PLAYER_PAUSED;
        return true;
    }
    if (p->state == PLAYER_PAUSED) {
        p->state = PLAYER_PLAYING;
        return true;
    }
    return false;
}

uint32_t player_accept(player_t *p, uint32_t n)
{
    if (p->state != PLAYER_PLAYING) {
        return 0;
    }
    uint32_t left = p->total_samples - p->position;
    if (n > left) {
        n = left;
    }
    p->position += n;
    if (p->position >= p->total_samples) {
        p->state = PLAYER_ENDED;
    }
    return n;
}

void player_fail(player_t *p)
{
    p->state = PLAYER_ERROR;
}

bool player_log_due(player_t *p)
{
    if (p->state == PLAYER_IDLE) {
        return false;
    }
    uint32_t sec = p->position / p->sample_rate;
    if (sec >= p->next_log_second) {
        p->next_log_second = sec + 1;
        return true;
    }
    return false;
}

uint32_t player_elapsed_ms(const player_t *p)
{
    return p->sample_rate ? (uint32_t)((uint64_t)p->position * 1000u / p->sample_rate) : 0;
}

uint32_t player_new_underruns(player_t *p, uint32_t total)
{
    uint32_t n = total - p->underruns_seen;   // unsigned wrap-safe
    p->underruns_seen = total;
    return n;
}

const char *player_state_name(player_state_t s)
{
    switch (s) {
    case PLAYER_IDLE: return "idle";
    case PLAYER_PLAYING: return "playing";
    case PLAYER_PAUSED: return "paused";
    case PLAYER_ENDED: return "ended";
    case PLAYER_ERROR: return "error";
    }
    return "?";
}

// ---- text ----------------------------------------------------------------------------------------------------
void text_mmss(char *buf, size_t n, uint32_t ms)
{
    if (!n) {
        return;
    }
    uint32_t s = ms / 1000u;
    if (s >= 3600u) {
        snprintf(buf, n, "%u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
    } else {
        snprintf(buf, n, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
    }
}

void text_fold_ascii(char *dst, size_t n, const char *src)
{
    // U+00C0..U+00FF without accents ('?' where there is no plain letter).
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUY?saaaaaaaceeeeiiiidnooooo/ouuuuy?y";
    if (!n) {
        return;
    }
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && o + 1 < n) {
        unsigned c = *s;
        if (c < 0x80) {
            dst[o++] = (char)c;
            s++;
            continue;
        }
        // Length of this UTF-8 sequence; anything malformed is treated as a single bad byte.
        size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        for (size_t k = 1; k < len; k++) {
            if ((s[k] & 0xC0) != 0x80) {
                len = 1;
                break;
            }
        }
        char out = '?';
        if (len == 2) {
            unsigned cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
            if (cp >= 0xC0 && cp <= 0xFF) {
                out = latin1[cp - 0xC0];
            }
        }
        dst[o++] = out;
        s += len;
    }
    dst[o] = '\0';
}
