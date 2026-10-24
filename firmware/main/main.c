// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "game_config.h"
#include "hd_pet.h"
#include "bsp/esp_mosaico.h"
#include "iot_button.h"
#include "mosaico_game_app.h"
#include "mosaico_game_input.h"

/* HachimoDock pet runtime on ESP-Mosaico. The engine owns display, touch
 * polling, Iris startup and first-frame health acceptance; hd_pet owns the
 * wire protocol, state machine and rendering into its own framebuffer. */

static button_handle_t s_ai_button;

static void ai_button_event_cb(void *arg, void *usr_data) {
  (void) arg;
  const bool pressed = (bool) (intptr_t) usr_data;
  (void) mosaico_game_input_button(0, pressed, esp_timer_get_time());
  hd_pet_button_event(0, pressed);
}

static void on_event(const mosaico_device_event_t *event) {
  if (!event) return;
  if (event->type == MOSAICO_DEVICE_EVENT_POINTER
      || event->type == MOSAICO_DEVICE_EVENT_TOUCH) {
    hd_pet_pointer_event(event->x, event->y, event->pressed);
  } else if (event->type == MOSAICO_DEVICE_EVENT_BUTTON) {
    hd_pet_button_event(event->value, event->pressed);
  }
}

static esp_err_t on_start(void) {
  ESP_ERROR_CHECK(hd_pet_start());
  /* Boot (GPIO61) stays reserved for ROM entry; the AI key is the single
   * physical pet button and doubles as SW1 in the HachimoDock gesture map. */
  int button_count = 1;
  if (bsp_iot_button_create(&s_ai_button, &button_count, 1) == ESP_OK
      && s_ai_button != NULL) {
    (void) iot_button_register_cb(
      s_ai_button, BUTTON_PRESS_DOWN, NULL, ai_button_event_cb,
      (void *) (intptr_t) true);
    (void) iot_button_register_cb(
      s_ai_button, BUTTON_PRESS_UP, NULL, ai_button_event_cb,
      (void *) (intptr_t) false);
  }
  return ESP_OK;
}

static const mosaico_game_app_config_t s_config = {
  .tag = GAME_NAME,
  .window_title = GAME_NAME,
  .canvas_bind = GSP_GAME_BIND_GAME_CANVAS,
  .touch_points = 1,
  .target_fps = GAME_TICK_HZ,
  .gsp_bundle = gsp_bundle_config,
  .on_event = on_event,
  .on_start = on_start,
  .on_update = hd_pet_tick,
  .on_render = hd_pet_render,
};

void app_main(void) {
  /* The engine starts iris_ota_support_start() and marks the image healthy
   * after presenting its first frame; the OTA writer remains in Recovery. */
  esp_err_t result = mosaico_game_app_run(&s_config);
  ESP_ERROR_CHECK(result);
}
