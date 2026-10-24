/*
 * /nand pet storage: LittleFS mount plus the raw-appearance slot backend
 * (see hd_storage.c). The pet_p4_raw_assets_* backend API is declared in
 * pet/pet_p4_raw_assets.h.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hd_storage_mount(void);
bool hd_storage_ready(void);

#ifdef __cplusplus
}
#endif
