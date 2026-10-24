#include "pet_p4_touch.h"
#include "pet_p4_conversation.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "pet_p4_assets.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_touch_core.h"

#define PET_P4_TOUCH_SWIPE_THRESHOLD 48
#define PET_P4_TOUCH_LONG_PRESS_MS 700ULL
#define PET_P4_TOUCH_QUEUE_LENGTH 16
#define PET_P4_TOUCH_REACTION_MIN_MS 1200ULL

static const char *TAG = "pet-p4-touch";
static QueueHandle_t g_touch_queue;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
static unsigned int g_reaction_sequence;
static bool g_ready;
/* Decoder state is fed only by pet_p4_touch_hw_sample() on the engine task;
 * decoded events are drained by pet_p4_touch_process() on the main tick, and
 * the latest swipe is handed to main.c via pet_p4_touch_last_swipe(). */
static pet_p4_touch_decoder_t g_decoder;
static portMUX_TYPE g_swipe_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_swipe_pending;
static int g_swipe_dx;
static int g_swipe_dy;
static unsigned long long g_swipe_ts_ms;

esp_err_t pet_p4_touch_init(void) {
  if (g_ready) return ESP_OK;
  // The cst9220 panel is owned by the engine; this module only keeps decoder
  // state for injected samples, so setup is pure state initialization.
  pet_p4_touch_decoder_init(&g_decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
  g_touch_queue = xQueueCreate(PET_P4_TOUCH_QUEUE_LENGTH, sizeof(pet_p4_touch_event_t));
  if (!g_touch_queue) return ESP_ERR_NO_MEM;
  taskENTER_CRITICAL(&g_swipe_lock);
  g_swipe_pending = false;
  g_swipe_dx = 0;
  g_swipe_dy = 0;
  g_swipe_ts_ms = 0;
  taskEXIT_CRITICAL(&g_swipe_lock);
  g_ready = true;
  ESP_LOGI(TAG, "touch ready (engine-injected samples, square 480x480 identity mapping)");
  return ESP_OK;
}

bool pet_p4_touch_ready(void) {
  return g_ready;
}

static const char *touch_event_name(pet_p4_touch_gesture_t gesture) {
  if (gesture == PET_P4_TOUCH_TAP) return "screen.region.tap";
  if (gesture == PET_P4_TOUCH_LONG_PRESS) return "screen.region.long_press";
  if (gesture == PET_P4_TOUCH_SWIPE_LEFT) return "touch.swipe_left";
  if (gesture == PET_P4_TOUCH_SWIPE_RIGHT) return "touch.swipe_right";
  if (gesture == PET_P4_TOUCH_SWIPE_UP) return "touch.swipe_up";
  if (gesture == PET_P4_TOUCH_SWIPE_DOWN) return "touch.swipe_down";
  return "touch.unknown";
}

static bool gesture_is_swipe(pet_p4_touch_gesture_t gesture) {
  return gesture == PET_P4_TOUCH_SWIPE_LEFT
    || gesture == PET_P4_TOUCH_SWIPE_RIGHT
    || gesture == PET_P4_TOUCH_SWIPE_UP
    || gesture == PET_P4_TOUCH_SWIPE_DOWN;
}

void pet_p4_touch_hw_sample(int x, int y, bool pressed, unsigned long long now_ms) {
  pet_p4_touch_event_t event;
  int ui_x;
  int ui_y;
  if (!g_ready) return;
  if (!pressed) {
    // Releases keep the last known position, exactly like the removed poller.
    ui_x = g_decoder.current_x;
    ui_y = g_decoder.current_y;
  } else {
    // The square 480x480 panel maps identity onto the UI grid.
    pet_p4_touch_panel_to_ui(x, y, &ui_x, &ui_y);
  }
  if (!pet_p4_touch_decoder_update(&g_decoder, pressed, ui_x, ui_y, now_ms, &event)) return;
  if (gesture_is_swipe(event.gesture)) {
    taskENTER_CRITICAL(&g_swipe_lock);
    g_swipe_pending = true;
    g_swipe_dx = event.dx;
    g_swipe_dy = event.dy;
    g_swipe_ts_ms = (unsigned long long) event.ts_ms;
    taskEXIT_CRITICAL(&g_swipe_lock);
  }
  if (!g_touch_queue || xQueueSend(g_touch_queue, &event, 0) != pdTRUE) {
    atomic_fetch_add_explicit(&g_dropped_events, 1, memory_order_relaxed);
  }
}

bool pet_p4_touch_last_swipe(int *out_dx, int *out_dy, unsigned long long *out_since_ms) {
  bool pending;
  taskENTER_CRITICAL(&g_swipe_lock);
  pending = g_swipe_pending;
  if (pending) {
    if (out_dx) *out_dx = g_swipe_dx;
    if (out_dy) *out_dy = g_swipe_dy;
    if (out_since_ms) *out_since_ms = g_swipe_ts_ms;
    g_swipe_pending = false;
  }
  taskEXIT_CRITICAL(&g_swipe_lock);
  return pending;
}

static const char *choose_touch_family(
  const pet_p4_runtime_state_t *state,
  int x,
  char *family,
  size_t family_size
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  const char *directional = x < 214 ? "touch.left" : (x > 426 ? "touch.right" : "");
  if (directional[0] && catalog && pet_p4_asset_catalog_find_exact(catalog, directional) >= 0) {
    snprintf(family, family_size, "%s", directional);
    return family;
  }
  const char *first = (g_reaction_sequence++ % 2U) == 0 ? "touch.lick" : "touch.what";
  const char *second = strcmp(first, "touch.lick") == 0 ? "touch.what" : "touch.lick";
  if (catalog && pet_p4_asset_catalog_find_exact(catalog, first) >= 0) {
    snprintf(family, family_size, "%s", first);
  } else if (catalog && pet_p4_asset_catalog_find_exact(catalog, second) >= 0) {
    snprintf(family, family_size, "%s", second);
  } else {
    snprintf(family, family_size, "%s", first);
  }
  return family;
}

static uint64_t touch_reaction_duration_ms(
  const pet_p4_runtime_state_t *state,
  const char *family
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  int index = catalog ? pet_p4_asset_catalog_find_exact(catalog, family) : -1;
  if (index < 0 && catalog) index = pet_p4_asset_catalog_nth_prefix(catalog, "touch.", 0);
  uint64_t duration = PET_P4_TOUCH_REACTION_MIN_MS;
  if (catalog && index >= 0 && index < catalog->count) {
    const pet_p4_asset_entry_t *entry = &catalog->entries[index];
    if (entry->frame_duration_ms > 0) {
      duration = (uint64_t) entry->frames * entry->frame_duration_ms;
    } else if (entry->fps > 0) {
      duration = ((uint64_t) entry->frames * 1000ULL) / (uint64_t) entry->fps;
    }
  }
  if (duration < PET_P4_TOUCH_REACTION_MIN_MS) duration = PET_P4_TOUCH_REACTION_MIN_MS;
  return duration;
}

static void send_touch_event(
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event,
  const char *action,
  bool handled_locally
) {
  if (!send_line || !event) return;
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", "input/event");
  cJSON_AddNumberToObject(payload, "version", 1);
  cJSON_AddNumberToObject(payload, "seq", ++g_event_sequence);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddStringToObject(payload, "control", "screen.touch");
  cJSON_AddStringToObject(payload, "gesture", pet_p4_touch_gesture_name(event->gesture));
  cJSON_AddStringToObject(payload, "event", touch_event_name(event->gesture));
  cJSON_AddStringToObject(payload, "context", state ? state->screen_page : "main");
  cJSON_AddStringToObject(payload, "action", action ? action : "disabled");
  cJSON_AddNumberToObject(payload, "x", event->x);
  cJSON_AddNumberToObject(payload, "y", event->y);
  cJSON_AddNumberToObject(payload, "dx", event->dx);
  cJSON_AddNumberToObject(payload, "dy", event->dy);
  cJSON_AddNumberToObject(payload, "durationMs", (double) event->duration_ms);
  cJSON_AddBoolToObject(payload, "handledLocally", handled_locally);
  cJSON_AddNumberToObject(payload, "tsMs", (double) event->ts_ms);
  cJSON_AddNumberToObject(
    payload,
    "dropped",
    atomic_load_explicit(&g_dropped_events, memory_order_relaxed)
  );
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  if (line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void process_touch_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event
) {
  if (pet_p4_conversation_active(state)) {
    if (event->gesture == PET_P4_TOUCH_SWIPE_LEFT || event->gesture == PET_P4_TOUCH_SWIPE_UP)
      pet_p4_conversation_move(state, -1, event->ts_ms);
    else if (event->gesture == PET_P4_TOUCH_SWIPE_RIGHT || event->gesture == PET_P4_TOUCH_SWIPE_DOWN)
      pet_p4_conversation_move(state, 1, event->ts_ms);
    return;
  }
  bool handled_locally = false;
  char action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if (gesture_is_swipe(event->gesture)) {
    const bool main_open = state && strcmp(state->screen_page, "main") == 0;
    const char *next_page = main_open ? "components"
      : state && strcmp(state->screen_page, "app") == 0 ? "components"
      : "main";
    if (state) {
      if (main_open) pet_p4_miniapp_catalog_focus_active();
      snprintf(state->screen_page, sizeof(state->screen_page), "%s", next_page);
      state->last_update_ms += 1;
      handled_locally = true;
      snprintf(action, sizeof(action), "page_%s", next_page);
    }
  } else {
    const char *event_name = touch_event_name(event->gesture);
    if (state && strcmp(state->screen_page, "app") == 0) {
      handled_locally = pet_p4_miniapp_dispatch_input(
        event_name,
        event->ts_ms,
        action,
        sizeof(action)
      );
      if (handled_locally) state->last_update_ms += 1;
    }
    if (!handled_locally && state && strcmp(state->screen_page, "main") == 0) {
      char family[PET_P4_STATE_MAX];
      choose_touch_family(state, event->x, family, sizeof(family));
      handled_locally = pet_p4_state_request_touch(
        state,
        family,
        event->x,
        event->y,
        touch_reaction_duration_ms(state, family),
        event->ts_ms
      );
      if (handled_locally) snprintf(action, sizeof(action), "pet_touch");
    }
  }
  send_touch_event(state, send_line, ctx, event, action, handled_locally);
}

void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  pet_p4_touch_event_t event;
  if (!g_touch_queue) return;
  while (xQueueReceive(g_touch_queue, &event, 0) == pdTRUE) {
    process_touch_event(state, send_line, ctx, &event);
  }
}

unsigned int pet_p4_touch_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}
