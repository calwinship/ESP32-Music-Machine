// Playback logic (plain C, no ESP-IDF headers; host-tested): seek maths, the per-frame output gate, MP3 frame
// splitting, the player state machine, and text helpers for the display/log.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logic/tune_index.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- seeking -------------------------------------------------------------------------------------------------
// Tune sample t is decoded sample t + delay, i.e. frame (t + delay) / spf, at (t + delay) % spf inside it.
// Decoding starts earlier (index entry for t's second: bit-reservoir + IMDCT pre-roll) and everything before the
// target is thrown away, so the first sample played is exactly t.
typedef struct {
    uint32_t target_sample;   // tune sample that plays first
    uint32_t byte_offset;     // where to start reading the file
    uint32_t start_frame;     // frame number at byte_offset
    uint32_t target_frame;    // frame that contains target_sample
    uint32_t skip_in_target;  // samples of target_frame's output to drop
} seek_plan_t;

// false if target_sample >= total_samples.
bool seek_plan(const tune_info_t *t, uint32_t target_sample, seek_plan_t *out);
uint32_t seek_ms_to_sample(uint32_t ms, uint32_t sample_rate);   // rounds down

// Counts the frames fed to the decoder (one call per frame, even if it produced nothing) and says which decoded
// samples to keep. Pre-roll frames may fail to decode (missing reservoir); their output is dropped either way.
typedef struct {
    uint32_t frame;
    uint32_t target_frame;
    uint32_t skip;
} frame_gate_t;

void frame_gate_init(frame_gate_t *g, const seek_plan_t *p);
// Returns how many of `decoded` samples to keep; *first = index of the first one.
uint32_t frame_gate_next(frame_gate_t *g, uint32_t decoded, uint32_t *first);
bool frame_gate_in_preroll(const frame_gate_t *g);   // true while the next frame is still before the target

// ---- speeds (plays) ----------------------------------------------------------------------------------------
// Each marker starts one play of the tune ("speed", slowest first). Speed i runs from marker i to marker i+1 (the
// last to the end of the tune). Before the first marker (lead-in) counts as speed 0 at progress 0. A tune without
// markers is one speed covering the whole tune.
typedef struct {
    uint8_t index;    // 0-based
    uint8_t count;    // >= 1
    uint32_t start;   // tune samples
    uint32_t end;     // exclusive
} speed_t;

void speed_at(const tune_info_t *t, uint32_t position, speed_t *out);
uint32_t speed_start(const tune_info_t *t, uint8_t index);   // index clamped to the last speed
uint16_t speed_permille(const speed_t *s, uint32_t position); // progress through the speed, 0..1000
// "Slow"/"Fast" for 2 speeds, "Slow"/"Medium"/"Fast" for 3, "" for 1; NULL otherwise (caller shows a number).
const char *speed_name(uint8_t index, uint8_t count);
// Swipe hints: is there a slower (*prev) / faster (*next) speed to swipe to? Changing speed clamps, so the first
// speed has no prev and the last no next; a single-speed tune has neither.
void speed_hints(uint8_t index, uint8_t count, bool *prev, bool *next);

// Resume: find the tune with this ID and turn a saved position into a sample. A position at or past the end
// (saved when the tune ended, or the tune got shorter after a re-sync) gives the tune's first marker. False if the
// tune is gone, so the caller falls back to the first tune.
bool resume_find(const tune_index_t *ix, uint32_t tune_id, uint32_t position_ms, uint16_t *tune,
                 uint32_t *sample);

// ---- MP3 frames ----------------------------------------------------------------------------------------------
// Length in bytes of the MPEG-1 Layer III frame whose 4-byte header is at p, or 0 if p is not such a header.
uint32_t mp3_frame_len(const uint8_t *p);

// ---- player state --------------------------------------------------------------------------------------------
typedef enum { PLAYER_IDLE, PLAYER_PLAYING, PLAYER_PAUSED, PLAYER_ENDED, PLAYER_ERROR } player_state_t;

typedef struct {
    player_state_t state;
    uint32_t sample_rate;
    uint32_t total_samples;
    uint32_t position;          // tune sample that plays next
    uint32_t next_log_second;   // position log fires when position reaches this second
    uint32_t underruns_seen;
} player_t;

void player_init(player_t *p);
void player_start(player_t *p, uint32_t sample_rate, uint32_t total_samples, uint32_t start_sample);
// Tap: PLAYING <-> PAUSED. Returns true if the state changed (no effect when idle, ended or failed).
bool player_toggle(player_t *p);
// n decoded samples are ready: returns how many to output (fewer at the end of the tune), advances the position
// and enters PLAYER_ENDED at the last sample. Returns 0 unless PLAYING.
uint32_t player_accept(player_t *p, uint32_t n);
void player_fail(player_t *p);
// Jump to tune sample `target` (clamped to the last sample). PLAYING and PAUSED stay as they are; after the end
// the player becomes PAUSED at the new position. Returns false (no effect) when idle or failed.
bool player_seek(player_t *p, uint32_t target);
// True once for each whole second the position reaches (the first call after start included).
bool player_log_due(player_t *p);
uint32_t player_elapsed_ms(const player_t *p);
// Feed the driver's running underrun count; returns how many are new since the last call.
uint32_t player_new_underruns(player_t *p, uint32_t total);
const char *player_state_name(player_state_t s);

// ---- text ----------------------------------------------------------------------------------------------------
// "m:ss" (or "h:mm:ss" from one hour), always NUL-terminated.
void text_mmss(char *buf, size_t n, uint32_t ms);
// UTF-8 -> ASCII for the display font: Latin-1 letters lose their accents (Nóra Chríonna -> Nora Chrionna),
// anything else non-ASCII becomes '?'. Invalid UTF-8 is handled. Always NUL-terminated.
void text_fold_ascii(char *dst, size_t n, const char *src);
// Code point -> UTF-8 in out[0..4] (NUL-terminated). Returns the byte count, 0 for an invalid code point.
size_t text_utf8(uint32_t cp, char out[5]);

#ifdef __cplusplus
}
#endif
