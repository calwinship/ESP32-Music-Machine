// The player: one task that reads tunes by path under BSP_TUNES_PATH, decodes MP3 and feeds I2S.
// Other tasks talk to it only through the functions below (a command queue), so it owns all player state.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logic/playback.h"

typedef struct {
    char title[TUNE_INDEX_MAX_TITLE + 1];   // UTF-8 (or an error message)
    uint32_t tune_id;
    uint16_t icon;                          // FontAwesome code point (tools/icons.txt)
    uint32_t colour;                        // tune theme colour 0xRRGGBB
    uint32_t elapsed_ms;                    // position in the recording
    uint32_t total_ms;
    player_state_t state;
    uint16_t tune;                          // 0-based, in title order
    uint16_t tune_count;
    uint8_t speed;                          // current play (0-based) and how many the tune has
    uint8_t speed_count;
    uint16_t speed_permille;                // progress through the current play
} player_status_t;

// resume: start paused at this tune/position if the tune still exists; otherwise play the first tune from its
// first marker.
void player_task_start(bool resume, uint32_t resume_tune_id, uint32_t resume_ms);
// All safe from any task (e.g. LVGL callbacks). Commands are dropped if the queue is full.
void player_task_tap(void);                     // play/pause; after the end: restart from the first marker
void player_task_restart_speed(void);           // back to the start of the current play, and play
void player_task_change_speed(int32_t delta);   // +1 next play, -1 previous; keeps play/pause
void player_task_seek(int32_t clicks);          // +/- SEEK_STEP_MS per click (logic/controls.h)
void player_task_select_tune(uint16_t tune);    // play this tune from its first marker
void player_status_get(player_status_t *out);   // consistent snapshot
// Tune list for the UI (valid once tune_count in the status is > 0; the index never changes after that).
bool player_tune_info(uint16_t tune, char *title, size_t n, uint32_t *colour);   // UTF-8 title, 0xRRGGBB
