// The player: one task that reads tunes by path under BSP_TUNES_PATH, decodes MP3 and feeds I2S.
// Other tasks talk to it only through the functions below (a command queue), so it owns all player state.
#pragma once
#include <stdint.h>

#include "logic/playback.h"

typedef struct {
    char title[TUNE_INDEX_MAX_TITLE + 1];   // accents folded to ASCII for the display font
    uint32_t elapsed_ms;                    // position in the recording
    player_state_t state;
    uint16_t tune;                          // 0-based, in title order
    uint16_t tune_count;
} player_status_t;

void player_task_start(void);
// All safe from any task (e.g. LVGL callbacks). Commands are dropped if the queue is full.
void player_task_tap(void);                     // play/pause; after the end: restart from the first marker
void player_task_seek(int32_t clicks);          // +/- SEEK_STEP_MS per click (logic/controls.h)
void player_task_change_tune(int32_t delta);    // +1 next, -1 previous (wraps); starts at its first marker
void player_status_get(player_status_t *out);   // consistent snapshot
