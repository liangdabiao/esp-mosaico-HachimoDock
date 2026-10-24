/*
 * Application-owned USB CDC pet link (see hd_cdc.c header comment).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hd_cdc_rx_fn)(const uint8_t *data, size_t len, void *ctx);

esp_err_t hd_cdc_init(hd_cdc_rx_fn rx_fn, void *rx_ctx);
bool hd_cdc_host_open(void);
void hd_cdc_write_all(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
