// USB link to ./sync-tunes: answers info requests while playing, and runs sync mode (SD card writes). See link_task.c.
#pragma once
#include <stdbool.h>

// True once per boot if the previous run asked for sync mode (clears the request).
bool link_sync_requested(void);
// Sync mode: "Syncing tunes" screen + SD writes from ./sync-tunes. Never returns (restarts into the player).
void link_sync_run(void);
// Player mode: start answering ./sync-tunes. Call after storage and the player are up.
void link_task_start(void);
