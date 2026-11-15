// The player: one task that reads a tune by path under BSP_TUNES_PATH, decodes MP3 and feeds I2S.
#pragma once
#include <stdint.h>

#include "logic/playback.h"

typedef struct {
    char title[TUNE_INDEX_MAX_TITLE + 1];   // accents folded to ASCII for the display font
    uint32_t elapsed_ms;
    player_state_t state;
} player_status_t;

void player_task_start(void);
void player_task_tap(void);                       // toggle play/pause; safe from any task (e.g. LVGL callbacks)
void player_status_get(player_status_t *out);     // consistent snapshot; safe from any task
