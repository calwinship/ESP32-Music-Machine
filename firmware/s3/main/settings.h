// Settings kept in NVS (namespace "player"): volume, where to resume, whether the guide was seen. NVS is never touched by ./sync-tunes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void settings_init(void);                                   // nvs_flash_init (erases only an unusable partition)
int32_t settings_volume(void);                              // VOLUME_DEFAULT if unset or invalid
void settings_save_volume(int32_t volume);
bool settings_resume(uint32_t *tune_id, uint32_t *position_ms);   // false if never saved
void settings_save_resume(uint32_t tune_id, uint32_t position_ms);
bool settings_guide_seen(void);                             // false until settings_save_guide_seen()
void settings_save_guide_seen(void);
