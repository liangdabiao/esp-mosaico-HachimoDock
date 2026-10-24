#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_touch_init(void);
bool pet_p4_touch_ready(void);
/* Feed one touch sample in panel coordinates (480x480 square panel, mapped
 * identity onto the UI) from the engine task; task context, not ISR. Gestures
 * are decoded here and dispatched later via pet_p4_touch_process(). */
void pet_p4_touch_hw_sample(int x, int y, bool pressed, unsigned long long now_ms);
/* Non-blocking read of the most recent swipe delta decoded since the previous
 * call; returns false when no new swipe arrived. Clears on read. */
bool pet_p4_touch_last_swipe(int *out_dx, int *out_dy, unsigned long long *out_since_ms);
void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
unsigned int pet_p4_touch_dropped_events(void);

#ifdef __cplusplus
}
#endif
