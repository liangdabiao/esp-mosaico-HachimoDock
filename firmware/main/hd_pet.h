/*
 * Pet runtime lifecycle bound to the mosaico_game_app callbacks (hd_pet.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hd_pet_start(void);
void hd_pet_tick(void);
void hd_pet_render(void);
void hd_pet_pointer_event(int32_t x, int32_t y, bool pressed);
void hd_pet_button_event(int32_t button, bool pressed);
void hd_pet_touch_swipe_as_joystick(int dx, int dy);

#ifdef __cplusplus
}
#endif
