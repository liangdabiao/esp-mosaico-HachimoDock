/*
 * [Input] Compile-time platform switch driven by the host simulator define.
 * [Output] One include that provides ESP-IDF primitives on the device and
 *          minimal standalone shims for the host simulator build.
 * [Pos] HachimoDock port platform boundary; every shared pet source includes
 *       this instead of reaching for ESP-IDF headers directly.
 * [Sync] Host stub implementations live in pet_host_shims.c (sim sources).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef MOSAICO_HOST_SIMULATION

/* ---- Host simulator: standalone shims (implemented in pet_host_shims.c) ---- */

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_SIZE 0x108
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_RESPONSE 0x10c

unsigned long long pet_host_time_ms(void);
#define esp_timer_get_time() ((int64_t) pet_host_time_ms() * 1000LL)

/* Heap caps collapse to plain malloc on the host. */
#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
void *pet_host_malloc(size_t size);
void *pet_host_calloc(size_t count, size_t size);
void pet_host_free(void *ptr);
#define heap_caps_malloc(size, caps) pet_host_malloc(size)
#define heap_caps_calloc(count, size, caps) pet_host_calloc(count, size)
#define heap_caps_free(ptr) pet_host_free(ptr)
#define heap_caps_aligned_free(ptr) pet_host_free(ptr)

/* Zero-cost logging that still references every argument so host-only
 * variables used solely in log calls do not trip -Werror=unused-variable. */
#define ESP_LOGI(tag, format, ...)   do { if (0) { (void) (tag); (void) printf(format, ##__VA_ARGS__); } } while (0)
#define ESP_LOGW(tag, format, ...)   do { if (0) { (void) (tag); (void) printf(format, ##__VA_ARGS__); } } while (0)
#define ESP_LOGE(tag, format, ...)   do { if (0) { (void) (tag); (void) printf(format, ##__VA_ARGS__); } } while (0)
#define ESP_LOGD(tag, format, ...)   do { if (0) { (void) (tag); (void) printf(format, ##__VA_ARGS__); } } while (0)

#define ESP_RETURN_ON_ERROR(err, tag, format, ...) \
  do { if ((err) != ESP_OK) return (err); } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, tag, format, ...) \
  do { if (!(expr)) return (err); } while (0)
#define ESP_ERROR_CHECK(err) ((void) (err))
#define ESP_ERROR_CHECK_WITHOUT_ABORT(err) ((void) (err))

/* FreeRTOS primitives the shared code touches degrade to no-ops on host. */
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static inline void portENTER_CRITICAL(portMUX_TYPE *mux) { (void) mux; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void) mux; }

/* Single-threaded host simulator: mutexes are pass-through placeholders. */
typedef void *SemaphoreHandle_t;
static inline void vTaskDelay(int ticks) { (void) ticks; }
#define portMAX_DELAY 0
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *) 1; }
static inline int xSemaphoreTake(SemaphoreHandle_t sem, int ticks) { (void) sem; (void) ticks; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t sem) { (void) sem; return 1; }

#else /* !MOSAICO_HOST_SIMULATION: real device build */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Device-only decode/present pipeline (H.264, hardware JPEG, PPA). */
#define PET_P4_DEVICE_PIPELINE 1

#endif /* MOSAICO_HOST_SIMULATION */
