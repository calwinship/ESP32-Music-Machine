#pragma once
#include "driver/sdmmc_host.h"
// Shared by bsp_sd.c and bsp_storage.c: the one place the SDMMC host and slot are configured.
void bsp_sd_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot);
