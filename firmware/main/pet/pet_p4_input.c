/*
 * [Input] Engine-injected SW1/SW2/SW3/shared center-key levels plus
 *         touch-derived joystick directions (the ESP-Mosaico board has no
 *         GPIO buttons, ADC joystick, or encoder of its own here) and
 *         validated desktop bindings.
 * [Output] Persisted configurable input actions, context-local previous/next
 *          selection plus SW2 top-level page toggling, center-aware joystick
 *          decoding with legacy encoder-event aliases, and component-local
 *          button routing that is active only while the component page is
 *          open, while the persisted global page-back gesture always wins and
 *          every other unmapped system-navigation gesture is suppressed, and
 *          package-authored navigation actions are ignored, plus correlated snapshots of the authoritative NVS-backed
 *          input configuration with versioned SW1-confirm/SW3-back and
 *          joystick-up/down Previous/Next defaults, and cached telemetry replay
 *          immediately after a mini-app is activated; conversation clearing
 *          preserves ongoing physical voice-hold feedback.
 * [Pos] ESP-Mosaico physical-input runtime.
 * [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.
 */

#include "pet_p4_input.h"
#include "pet_p4_conversation.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"

#include "pet_p4_audio.h"
#include "pet_p4_input_core.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_media.h"

#define PET_P4_INPUT_DEBOUNCE_MS 25
#define PET_P4_INPUT_LONG_PRESS_MS 700
#define PET_P4_INPUT_QUEUE_LENGTH 32
#define PET_P4_INPUT_NVS_NAMESPACE "pet_input"
#define PET_P4_INPUT_NVS_KEY "config"

typedef struct {
  char event[PET_P4_INPUT_EVENT_MAX];
  char action[PET_P4_INPUT_ACTION_MAX];
  char value[PET_P4_INPUT_VALUE_MAX];
} pet_p4_input_binding_t;

typedef struct {
  uint32_t version;
  uint32_t binding_count;
  pet_p4_input_binding_t bindings[PET_P4_INPUT_MAX_BINDINGS];
} pet_p4_input_config_t;

typedef enum {
  PET_P4_INPUT_GESTURE_DOWN,
  PET_P4_INPUT_GESTURE_UP,
  PET_P4_INPUT_GESTURE_SHORT_PRESS,
  PET_P4_INPUT_GESTURE_LONG_PRESS,
  PET_P4_INPUT_GESTURE_HOLD_START,
  PET_P4_INPUT_GESTURE_HOLD_END,
  PET_P4_INPUT_GESTURE_ROTATE,
  PET_P4_INPUT_GESTURE_DIRECTION,
} pet_p4_input_gesture_t;

typedef struct {
  pet_p4_input_control_t control;
  pet_p4_input_gesture_t gesture;
  int delta;
  unsigned long long ts_ms;
} pet_p4_input_event_t;

/* Button controls the engine can inject. Order matches the decoder/raw-level
 * arrays below; the shared center key keeps its historical encoder-press
 * identity so "button.encoder.*" events stay unchanged. */
#define PET_P4_INPUT_BUTTON_COUNT 4

static const pet_p4_input_control_t g_button_controls[PET_P4_INPUT_BUTTON_COUNT] = {
  PET_P4_INPUT_CONTROL_SW1,
  PET_P4_INPUT_CONTROL_SW2,
  PET_P4_INPUT_CONTROL_SW3,
  PET_P4_INPUT_CONTROL_ENCODER_PRESS,
};

static const char *TAG = "pet-p4-input";
static QueueHandle_t g_event_queue;
static pet_p4_input_config_t g_config;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
/* Raw injected levels cross from the engine task into the main-loop tick;
 * the decoders and tick timestamp below are owned by whichever task calls
 * pet_p4_input_process(). */
static atomic_bool g_button_pressed[PET_P4_INPUT_BUTTON_COUNT];
static pet_p4_button_decoder_t g_button_decoders[PET_P4_INPUT_BUTTON_COUNT];
static bool g_tick_started;
static unsigned long long g_last_tick_ms;

static void copy_text(char *dest, size_t dest_size, const char *src) {
  if (!dest || dest_size == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= dest_size) n = dest_size - 1;
  memcpy(dest, src, n);
  dest[n] = '\0';
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static bool event_is_allowed(const char *event) {
  static const char *const allowed[] = {
    "button.sw1.short_press",
    "button.sw1.long_press",
    "button.sw1.hold",
    "button.sw2.short_press",
    "button.sw2.long_press",
    "button.sw2.hold",
    "button.sw3.short_press",
    "button.sw3.long_press",
    "button.sw3.hold",
    "button.encoder.short_press",
    "button.encoder.long_press",
    "button.encoder.hold",
    "knob.rotate_cw",
    "knob.rotate_ccw",
    "joystick.up",
    "joystick.down",
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i += 1) {
    if (strcmp(event, allowed[i]) == 0) return true;
  }
  return false;
}

static bool action_is_allowed(const char *action) {
  static const char *const allowed[] = {
    "disabled",
    "voice_ptt",
    "realtime_chat",
    "agent_enter",
    "agent_prompt",
    "session_next",
    "session_previous",
    "session_clear",
    "miniapp_screen_tap",
    "miniapp_screen_long_press",
    "page_toggle",
    "page_enter",
    "page_back",
    "page_main",
    "page_app",
    "component_center",
    "miniapp_action",
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i += 1) {
    if (strcmp(action, allowed[i]) == 0) return true;
  }
  return false;
}

static bool binding_is_valid(const pet_p4_input_binding_t *binding) {
  if (!binding
      || strnlen(binding->event, sizeof(binding->event)) >= sizeof(binding->event)
      || strnlen(binding->action, sizeof(binding->action)) >= sizeof(binding->action)
      || strnlen(binding->value, sizeof(binding->value)) >= sizeof(binding->value)
      || !event_is_allowed(binding->event)
      || !action_is_allowed(binding->action)) {
    return false;
  }
  return true;
}

static void add_default_binding(
  pet_p4_input_config_t *config,
  const char *event,
  const char *action,
  const char *value
) {
  if (!config || config->binding_count >= PET_P4_INPUT_MAX_BINDINGS) return;
  pet_p4_input_binding_t *binding = &config->bindings[config->binding_count++];
  copy_text(binding->event, sizeof(binding->event), event);
  copy_text(binding->action, sizeof(binding->action), action);
  copy_text(binding->value, sizeof(binding->value), value);
}

static void load_default_config(pet_p4_input_config_t *config) {
  memset(config, 0, sizeof(*config));
  config->version = PET_P4_INPUT_CONFIG_VERSION;
  add_default_binding(config, "button.sw1.short_press", "page_enter", "");
  add_default_binding(config, "button.sw1.long_press", "disabled", "");
  add_default_binding(config, "button.sw1.hold", "voice_ptt", "");
  add_default_binding(config, "button.sw2.short_press", "component_center", "");
  add_default_binding(config, "button.sw2.long_press", "realtime_chat", "");
  add_default_binding(config, "button.sw2.hold", "disabled", "");
  add_default_binding(config, "button.sw3.short_press", "page_back", "");
  add_default_binding(config, "button.sw3.long_press", "disabled", "");
  add_default_binding(config, "button.sw3.hold", "disabled", "");
  add_default_binding(config, "button.encoder.short_press", "page_enter", "");
  add_default_binding(config, "button.encoder.long_press", "disabled", "");
  add_default_binding(config, "button.encoder.hold", "disabled", "");
  add_default_binding(config, "knob.rotate_cw", "session_next", "");
  add_default_binding(config, "knob.rotate_ccw", "session_previous", "");
  add_default_binding(config, "joystick.up", "session_previous", "");
  add_default_binding(config, "joystick.down", "session_next", "");
}

static bool config_bindings_are_valid(const pet_p4_input_config_t *config) {
  if (!config || config->binding_count > PET_P4_INPUT_MAX_BINDINGS) {
    return false;
  }
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    if (!binding_is_valid(&config->bindings[i])) return false;
  }
  return true;
}

static bool config_is_valid(const pet_p4_input_config_t *config) {
  return config
    && config->version == PET_P4_INPUT_CONFIG_VERSION
    && config_bindings_are_valid(config);
}

static void migrate_default_binding(
  pet_p4_input_config_t *config,
  const char *event,
  const char *old_action,
  const char *old_value,
  const char *new_action,
  const char *new_value
) {
  if (!config) return;
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    pet_p4_input_binding_t *binding = &config->bindings[i];
    if (strcmp(binding->event, event) != 0
        || strcmp(binding->action, old_action) != 0
        || strcmp(binding->value, old_value) != 0) {
      continue;
    }
    copy_text(binding->action, sizeof(binding->action), new_action);
    copy_text(binding->value, sizeof(binding->value), new_value);
    return;
  }
}

static bool migrate_v2_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 2 || !config_bindings_are_valid(config)) return false;

  // Version 2 shipped two successive default layouts. Only values matching
  // either old default are rewritten; unrelated user mappings stay intact.
  migrate_default_binding(config, "button.sw1.short_press", "page_toggle", "", "disabled", "");
  migrate_default_binding(config, "button.sw1.short_press", "agent_enter", "", "disabled", "");
  migrate_default_binding(config, "button.sw2.short_press", "agent_enter", "", "disabled", "");
  migrate_default_binding(
    config,
    "button.sw2.short_press",
    "agent_prompt",
    "继续当前任务并给出下一步。",
    "disabled",
    ""
  );
  migrate_default_binding(config, "button.sw3.short_press", "disabled", "", "component_center", "");
  migrate_default_binding(
    config,
    "button.sw3.long_press",
    "agent_prompt",
    "继续当前任务并给出下一步。",
    "disabled",
    ""
  );
  migrate_default_binding(config, "knob.rotate_cw", "disabled", "", "session_next", "");
  migrate_default_binding(config, "knob.rotate_ccw", "disabled", "", "session_previous", "");
  config->version = 3;
  return true;
}

static bool migrate_v3_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 3 || !config_bindings_are_valid(config)) return false;

  // Rewrite only bindings that still match version 3 defaults. User mappings
  // on all other gestures remain untouched.
  migrate_default_binding(
    config,
    "button.sw2.short_press",
    "disabled",
    "",
    "component_center",
    ""
  );
  migrate_default_binding(
    config,
    "button.sw3.short_press",
    "component_center",
    "",
    "page_back",
    ""
  );
  migrate_default_binding(
    config,
    "button.encoder.long_press",
    "page_back",
    "",
    "disabled",
    ""
  );
  config->version = 4;
  return true;
}

static bool migrate_v4_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 4 || !config_bindings_are_valid(config)) return false;

  // Version 5 adds the two directions that did not exist on the encoder.
  // Old left/right/press bindings keep their event names and user actions.
  add_default_binding(config, "joystick.up", "disabled", "");
  add_default_binding(config, "joystick.down", "disabled", "");
  config->version = 5;
  return true;
}

static bool migrate_v5_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 5 || !config_bindings_are_valid(config)) return false;

  // Version 6 promotes the unchanged SW1 short-press default to Confirm.
  // Rewrite only the former default so explicit user mappings remain intact.
  migrate_default_binding(
    config,
    "button.sw1.short_press",
    "disabled",
    "",
    "page_enter",
    ""
  );
  config->version = 6;
  return true;
}

static bool migrate_v6_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 6 || !config_bindings_are_valid(config)) return false;

  // Version 7 swaps only the shipped SW1/SW3 navigation defaults. Explicit
  // user mappings on either switch and every unrelated gesture stay intact.
  migrate_default_binding(
    config,
    "button.sw1.short_press",
    "page_enter",
    "",
    "page_back",
    ""
  );
  migrate_default_binding(
    config,
    "button.sw3.short_press",
    "page_back",
    "",
    "page_enter",
    ""
  );
  config->version = 7;
  return true;
}

static bool migrate_v7_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 7 || !config_bindings_are_valid(config)) return false;

  // Version 8 restores SW1 as Confirm and moves the global Back action to
  // SW3. Rewrite only the shipped version-7 values so unrelated user
  // mappings remain untouched.
  migrate_default_binding(
    config,
    "button.sw1.short_press",
    "page_back",
    "",
    "page_enter",
    ""
  );
  migrate_default_binding(
    config,
    "button.sw3.short_press",
    "page_enter",
    "",
    "page_back",
    ""
  );
  config->version = 8;
  return true;
}

static bool migrate_v8_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 8 || !config_bindings_are_valid(config)) return false;

  // Version 9 makes the two vertical joystick directions useful in the
  // top-level UI. Only the shipped Unbound values are rewritten; an existing
  // custom action on either direction is preserved.
  migrate_default_binding(
    config,
    "joystick.up",
    "disabled",
    "",
    "session_previous",
    ""
  );
  migrate_default_binding(
    config,
    "joystick.down",
    "disabled",
    "",
    "session_next",
    ""
  );
  config->version = PET_P4_INPUT_CONFIG_VERSION;
  return true;
}

static bool migrate_input_config(pet_p4_input_config_t *config) {
  if (!config) return false;
  if (config->version == 2 && !migrate_v2_config(config)) return false;
  if (config->version == 3 && !migrate_v3_config(config)) return false;
  if (config->version == 4 && !migrate_v4_config(config)) return false;
  if (config->version == 5 && !migrate_v5_config(config)) return false;
  if (config->version == 6 && !migrate_v6_config(config)) return false;
  if (config->version == 7 && !migrate_v7_config(config)) return false;
  return config->version == 8 && migrate_v8_config(config);
}

static esp_err_t persist_config(const pet_p4_input_config_t *config) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(PET_P4_INPUT_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, PET_P4_INPUT_NVS_KEY, config, sizeof(*config));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static void load_persisted_config(void) {
  nvs_handle_t handle;
  pet_p4_input_config_t stored;
  size_t size = sizeof(stored);
  load_default_config(&g_config);
  if (nvs_open(PET_P4_INPUT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
  esp_err_t err = nvs_get_blob(handle, PET_P4_INPUT_NVS_KEY, &stored, &size);
  nvs_close(handle);
  if (err == ESP_OK && size == sizeof(stored) && config_is_valid(&stored)) {
    g_config = stored;
    ESP_LOGI(TAG, "restored %u input bindings from NVS", (unsigned int) g_config.binding_count);
  } else if (err == ESP_OK && size == sizeof(stored)) {
    const uint32_t stored_version = stored.version;
    if (!migrate_input_config(&stored)) {
      ESP_LOGW(TAG, "ignored input config version %u", (unsigned int) stored_version);
      return;
    }
    esp_err_t persist_err = persist_config(&stored);
    g_config = stored;
    if (persist_err == ESP_OK) {
      ESP_LOGI(
        TAG,
        "migrated %u input bindings from config v%u",
        (unsigned int) g_config.binding_count,
        (unsigned int) stored_version
      );
    } else {
      ESP_LOGW(TAG, "using migrated input config without persistence: %s", esp_err_to_name(persist_err));
    }
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "ignored invalid input config: %s", esp_err_to_name(err));
  }
}

static pet_p4_input_binding_t *find_binding(pet_p4_input_config_t *config, const char *event) {
  if (!config || !event) return NULL;
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    if (strcmp(config->bindings[i].event, event) == 0) return &config->bindings[i];
  }
  return NULL;
}

static const pet_p4_input_binding_t *active_binding(const char *event) {
  return find_binding(&g_config, event);
}

static bool apply_binding_json(pet_p4_input_config_t *config, const cJSON *item, char *error, size_t error_size) {
  const char *event = json_string(item, "event");
  const char *action = json_string(item, "action");
  const char *value = json_string(item, "value");
  if (!event_is_allowed(event)) {
    snprintf(error, error_size, "unsupported input event: %s", event);
    return false;
  }
  if (!action_is_allowed(action)) {
    snprintf(error, error_size, "unsupported input action: %s", action);
    return false;
  }
  if (strlen(value) >= PET_P4_INPUT_VALUE_MAX) {
    snprintf(error, error_size, "input action value exceeds %d bytes", PET_P4_INPUT_VALUE_MAX - 1);
    return false;
  }
  pet_p4_input_binding_t *binding = find_binding(config, event);
  if (!binding) {
    if (config->binding_count >= PET_P4_INPUT_MAX_BINDINGS) {
      snprintf(error, error_size, "too many input bindings");
      return false;
    }
    binding = &config->bindings[config->binding_count++];
    memset(binding, 0, sizeof(*binding));
    copy_text(binding->event, sizeof(binding->event), event);
  }
  copy_text(binding->action, sizeof(binding->action), action);
  copy_text(binding->value, sizeof(binding->value), value);
  return true;
}

static void send_config_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  bool legacy,
  const char *request_id,
  bool ok,
  unsigned int binding_count,
  const char *error
) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", legacy ? "button-config-ack" : "input/config-ack");
  cJSON_AddStringToObject(payload, "requestId", request_id ? request_id : "");
  cJSON_AddBoolToObject(payload, "ok", ok);
  cJSON_AddNumberToObject(payload, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddNumberToObject(payload, "bindingCount", binding_count);
  cJSON_AddBoolToObject(payload, "persisted", ok);
  if (error && error[0]) {
    cJSON_AddStringToObject(payload, "error", error);
    cJSON_AddStringToObject(payload, "message", error);
  } else {
    cJSON_AddStringToObject(payload, "message", "input config written");
  }
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

bool pet_p4_input_handle_config(
  const cJSON *payload,
  bool legacy_button_config,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(payload, "requestId");
  const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(payload, "bindings");
  const cJSON *reset = cJSON_GetObjectItemCaseSensitive(payload, "reset");
  pet_p4_input_config_t *next = heap_caps_malloc(
    sizeof(*next),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  char error[128] = {0};

  if (!request_id[0]) request_id = json_string(payload, "request_id");
  if (!next) next = malloc(sizeof(*next));
  if (!next) {
    snprintf(error, sizeof(error), "input config allocation failed");
    send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
    return false;
  }
  load_default_config(next);
  if (!cJSON_IsTrue(reset)) {
    if (!cJSON_IsArray(bindings)) {
      snprintf(error, sizeof(error), "bindings array is required");
      free(next);
      send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
      return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, bindings) {
      if (!cJSON_IsObject(item) || !apply_binding_json(next, item, error, sizeof(error))) {
        if (!error[0]) snprintf(error, sizeof(error), "invalid input binding");
        free(next);
        send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
        return false;
      }
    }
  }

  esp_err_t err = persist_config(next);
  if (err != ESP_OK) {
    snprintf(error, sizeof(error), "persist input config failed: %s", esp_err_to_name(err));
    free(next);
    send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
    return false;
  }
  g_config = *next;
  free(next);
  send_config_ack(
    send_line,
    ctx,
    legacy_button_config,
    request_id,
    true,
    (unsigned int) g_config.binding_count,
    NULL
  );
  ESP_LOGI(TAG, "applied %u input bindings", (unsigned int) g_config.binding_count);
  return true;
}

bool pet_p4_input_send_config_state(
  const cJSON *request,
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(request, "requestId");
  const char *voice_button = "";
  bool voice_enabled = false;
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  cJSON *config = cJSON_CreateObject();
  cJSON *bindings = cJSON_CreateArray();
  char *line;
  bool sent;

  if (!root || !payload || !config || !bindings) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    cJSON_Delete(config);
    cJSON_Delete(bindings);
    return false;
  }

  for (uint32_t i = 0; i < g_config.binding_count; i += 1) {
    const pet_p4_input_binding_t *binding = &g_config.bindings[i];
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      cJSON_Delete(root);
      cJSON_Delete(payload);
      cJSON_Delete(config);
      cJSON_Delete(bindings);
      return false;
    }
    cJSON_AddStringToObject(item, "event", binding->event);
    cJSON_AddStringToObject(item, "action", binding->action);
    cJSON_AddStringToObject(item, "value", binding->value);
    cJSON_AddItemToArray(bindings, item);
    if (!voice_enabled && strcmp(binding->action, "voice_ptt") == 0) {
      voice_enabled = true;
      voice_button = binding->event;
    }
  }

  cJSON_AddNumberToObject(config, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddBoolToObject(config, "voiceEnabled", voice_enabled);
  cJSON_AddStringToObject(config, "voiceButton", voice_button);
  cJSON_AddItemToObject(config, "bindings", bindings);
  cJSON_AddStringToObject(payload, "requestId", request_id);
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddStringToObject(payload, "boardDeviceId", board_device_id ? board_device_id : "");
  cJSON_AddStringToObject(payload, "runtime", "esp-p4");
  cJSON_AddNumberToObject(payload, "bindingCount", g_config.binding_count);
  cJSON_AddItemToObject(payload, "config", config);
  cJSON_AddStringToObject(root, "topic", "input/config-state");
  cJSON_AddItemToObject(root, "payload", payload);

  line = cJSON_PrintUnformatted(root);
  sent = line && send_line;
  if (sent) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
  return sent;
}

static const char *control_name(pet_p4_input_control_t control) {
  switch (control) {
    case PET_P4_INPUT_CONTROL_SW1: return "key.1";
    case PET_P4_INPUT_CONTROL_SW2: return "key.2";
    case PET_P4_INPUT_CONTROL_SW3: return "key.3";
    case PET_P4_INPUT_CONTROL_ENCODER_PRESS: return "encoder.press";
    case PET_P4_INPUT_CONTROL_ENCODER: return "encoder";
    case PET_P4_INPUT_CONTROL_JOYSTICK: return "joystick";
    default: return "unknown";
  }
}

static const char *button_event_prefix(pet_p4_input_control_t control) {
  switch (control) {
    case PET_P4_INPUT_CONTROL_SW1: return "button.sw1";
    case PET_P4_INPUT_CONTROL_SW2: return "button.sw2";
    case PET_P4_INPUT_CONTROL_SW3: return "button.sw3";
    case PET_P4_INPUT_CONTROL_ENCODER_PRESS: return "button.encoder";
    default: return "";
  }
}

static void queue_event(
  pet_p4_input_control_t control,
  pet_p4_input_gesture_t gesture,
  int delta
) {
  pet_p4_input_event_t event = {
    .control = control,
    .gesture = gesture,
    .delta = delta,
    .ts_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL),
  };
  if (!g_event_queue || xQueueSend(g_event_queue, &event, 0) != pdTRUE) {
    atomic_fetch_add_explicit(&g_dropped_events, 1, memory_order_relaxed);
  }
}

static void queue_button_flags(pet_p4_input_control_t control, uint8_t flags) {
  if (flags & PET_P4_BUTTON_EVENT_SHORT_PRESS) queue_event(control, PET_P4_INPUT_GESTURE_SHORT_PRESS, 0);
  if (flags & PET_P4_BUTTON_EVENT_LONG_PRESS) {
    queue_event(control, PET_P4_INPUT_GESTURE_LONG_PRESS, 0);
    queue_event(control, PET_P4_INPUT_GESTURE_HOLD_START, 0);
  }
  if (flags & PET_P4_BUTTON_EVENT_LONG_RELEASE) {
    queue_event(control, PET_P4_INPUT_GESTURE_HOLD_END, 0);
  }
}

void pet_p4_input_hw_event(int control, bool pressed) {
  switch ((pet_p4_input_control_t) control) {
    case PET_P4_INPUT_CONTROL_SW1:
    case PET_P4_INPUT_CONTROL_SW2:
    case PET_P4_INPUT_CONTROL_SW3:
    case PET_P4_INPUT_CONTROL_ENCODER_PRESS:
      for (size_t i = 0; i < PET_P4_INPUT_BUTTON_COUNT; i += 1) {
        if (g_button_controls[i] == (pet_p4_input_control_t) control) {
          atomic_store_explicit(&g_button_pressed[i], pressed, memory_order_relaxed);
          return;
        }
      }
      return;
    case PET_P4_INPUT_CONTROL_JOYSTICK_UP:
    case PET_P4_INPUT_CONTROL_JOYSTICK_DOWN:
    case PET_P4_INPUT_CONTROL_JOYSTICK_LEFT:
    case PET_P4_INPUT_CONTROL_JOYSTICK_RIGHT:
      // A swipe arrives as one completed direction pulse; the main tick maps
      // it onto the legacy joystick/knob event names.
      if (pressed) {
        pet_p4_joystick_direction_t direction = PET_P4_JOYSTICK_CENTER;
        switch ((pet_p4_input_control_t) control) {
          case PET_P4_INPUT_CONTROL_JOYSTICK_UP:
            direction = PET_P4_JOYSTICK_UP;
            break;
          case PET_P4_INPUT_CONTROL_JOYSTICK_DOWN:
            direction = PET_P4_JOYSTICK_DOWN;
            break;
          case PET_P4_INPUT_CONTROL_JOYSTICK_LEFT:
            direction = PET_P4_JOYSTICK_LEFT;
            break;
          case PET_P4_INPUT_CONTROL_JOYSTICK_RIGHT:
            direction = PET_P4_JOYSTICK_RIGHT;
            break;
          default:
            break;
        }
        if (direction != PET_P4_JOYSTICK_CENTER) {
          queue_event(PET_P4_INPUT_CONTROL_JOYSTICK, PET_P4_INPUT_GESTURE_DIRECTION, (int) direction);
        }
      }
      return;
    default:
      // No rotary encoder or analog joystick exists on this board, so raw
      // ENCODER quadrature and analog JOYSTICK controls have no source.
      return;
  }
}

static void advance_button_decoders(void) {
  const unsigned long long now_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
  uint32_t elapsed_ms = 0;
  if (g_tick_started && now_ms > g_last_tick_ms) {
    elapsed_ms = (uint32_t) (now_ms - g_last_tick_ms);
  }
  g_tick_started = true;
  g_last_tick_ms = now_ms;
  if (elapsed_ms == 0) return;
  for (size_t i = 0; i < PET_P4_INPUT_BUTTON_COUNT; i += 1) {
    uint8_t flags = pet_p4_button_decoder_update(
      &g_button_decoders[i],
      atomic_load_explicit(&g_button_pressed[i], memory_order_relaxed),
      elapsed_ms
    );
    queue_button_flags(g_button_controls[i], flags);
  }
}

esp_err_t pet_p4_input_init(void) {
  load_persisted_config();
  // Raw levels arrive through pet_p4_input_hw_event(); decoding itself runs in
  // pet_p4_input_process() on the main tick, so only state needs setup here.
  for (size_t i = 0; i < PET_P4_INPUT_BUTTON_COUNT; i += 1) {
    atomic_store_explicit(&g_button_pressed[i], false, memory_order_relaxed);
    pet_p4_button_decoder_init(
      &g_button_decoders[i],
      false,
      PET_P4_INPUT_DEBOUNCE_MS,
      PET_P4_INPUT_LONG_PRESS_MS
    );
  }
  g_tick_started = false;
  g_last_tick_ms = 0;
  g_event_queue = xQueueCreate(PET_P4_INPUT_QUEUE_LENGTH, sizeof(pet_p4_input_event_t));
  if (!g_event_queue) return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG, "inputs ready (engine-injected sw1/sw2/sw3/center-key plus touch directions)");
  return ESP_OK;
}

esp_err_t pet_p4_input_reset_config(void) {
  pet_p4_input_config_t defaults;
  load_default_config(&defaults);
  esp_err_t err = persist_config(&defaults);
  if (err == ESP_OK) g_config = defaults;
  return err;
}

static void send_input_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture,
  const pet_p4_input_binding_t *binding,
  const char *action_override,
  bool handled_locally
) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  const char *resolved_action = action_override && action_override[0]
    ? action_override
    : (binding ? binding->action : "disabled");
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", "input/event");
  cJSON_AddNumberToObject(payload, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddNumberToObject(payload, "seq", ++g_event_sequence);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddStringToObject(payload, "control", control_name(event->control));
  cJSON_AddStringToObject(payload, "gesture", gesture ? gesture : "");
  cJSON_AddNumberToObject(payload, "delta", event->delta);
  cJSON_AddStringToObject(payload, "event", event_name ? event_name : "");
  cJSON_AddStringToObject(payload, "context", state ? state->screen_page : "main");
  cJSON_AddStringToObject(payload, "action", resolved_action);
  if (state
      && (strcmp(resolved_action, "session_next") == 0
          || strcmp(resolved_action, "session_previous") == 0)
      && state->current_session_index > 0
      && state->current_session_index <= state->session_queue_count) {
    const pet_p4_session_queue_item_t *selected =
      &state->session_queue[state->current_session_index - 1];
    cJSON_AddStringToObject(payload, "sessionId", selected->id);
    cJSON_AddStringToObject(payload, "sessionTitle", selected->title);
    cJSON_AddNumberToObject(payload, "sessionIndex", state->current_session_index);
    cJSON_AddNumberToObject(payload, "sessionCount", state->session_queue_count);
  }
  if (binding && binding->value[0]) cJSON_AddStringToObject(payload, "value", binding->value);
  cJSON_AddBoolToObject(payload, "handledLocally", handled_locally);
  cJSON_AddNumberToObject(payload, "tsMs", (double) event->ts_ms);
  cJSON_AddNumberToObject(
    payload,
    "dropped",
    atomic_load_explicit(&g_dropped_events, memory_order_relaxed)
  );
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static bool apply_local_action(
  pet_p4_runtime_state_t *state,
  const pet_p4_input_binding_t *binding,
  unsigned long long ts_ms,
  char *action_override,
  size_t action_override_size
) {
  if (!binding || strcmp(binding->action, "disabled") == 0) return true;
  if (!state) return false;
  if (strcmp(binding->action, "session_clear") == 0) {
    memset(state->session_queue, 0, sizeof(state->session_queue));
    state->session_queue_count = 0;
    state->current_session_index = 0;
    state->current_session_count = 0;
    state->current_session_title[0] = '\0';
    state->current_session_notice[0] = '\0';
    state->session_notice_until_ms = 0;
    // Clearing conversation cards must not hide an ongoing voice hold.
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "session_next") == 0
      || strcmp(binding->action, "session_previous") == 0) {
    const bool main_open = strcmp(state->screen_page, "main") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!main_open && !center_open) return false;
    const int direction = strcmp(binding->action, "session_previous") == 0 ? -1 : 1;
    if (center_open) {
      if (!pet_p4_miniapp_catalog_move(direction)) return false;
      copy_text(action_override, action_override_size, "component_select");
      state->last_update_ms = ts_ms;
      return true;
    }
    if (state->session_queue_count == 0) return false;
    unsigned int selected = state->current_session_index > 0
      ? state->current_session_index - 1
      : 0;
    if (selected >= state->session_queue_count) selected = 0;
    if (direction < 0) {
      selected = selected == 0 ? state->session_queue_count - 1 : selected - 1;
    } else {
      selected = (selected + 1) % state->session_queue_count;
    }
    state->current_session_index = selected + 1;
    state->current_session_count = state->session_queue_count;
    copy_text(
      state->current_session_id,
      sizeof(state->current_session_id),
      state->session_queue[selected].id
    );
    copy_text(
      state->current_session_title,
      sizeof(state->current_session_title),
      state->session_queue[selected].title
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_toggle") == 0) {
    const bool app_open = strcmp(state->screen_page, "app") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!app_open && !center_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      app_open ? "components" : (center_open ? "main" : "components")
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_enter") == 0) {
    if (strcmp(state->screen_page, "app") == 0) return false;
    if (strcmp(state->screen_page, "components") == 0) {
      if (!pet_p4_miniapp_catalog_activate_selected()) return false;
      pet_p4_miniapp_sync_stats(&state->stats);
      copy_text(state->screen_page, sizeof(state->screen_page), "app");
    } else return false;
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_back") == 0) {
    const bool app_open = strcmp(state->screen_page, "app") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!app_open && !center_open) return false;
    if (app_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      app_open ? "components" : "main"
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_main") == 0) {
    copy_text(state->screen_page, sizeof(state->screen_page), "main");
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "component_center") == 0) {
    const bool main_open = strcmp(state->screen_page, "main") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!main_open && !center_open) return false;
    if (main_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      center_open ? "main" : "components"
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_app") == 0) {
    if (!pet_p4_miniapp_active()) return false;
    copy_text(state->screen_page, sizeof(state->screen_page), "app");
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "miniapp_action") == 0) {
    if (strcmp(state->screen_page, "app") != 0 || !binding->value[0]) return false;
    if (!pet_p4_miniapp_dispatch_action(binding->value, ts_ms)) return false;
    copy_text(action_override, action_override_size, binding->value);
    state->last_update_ms = ts_ms;
    return true;
  }
  const char *miniapp_event = NULL;
  if (strcmp(binding->action, "miniapp_screen_tap") == 0) {
    miniapp_event = "screen.region.tap";
  } else if (strcmp(binding->action, "miniapp_screen_long_press") == 0) {
    miniapp_event = "screen.region.long_press";
  }
  if (miniapp_event) {
    if (strcmp(state->screen_page, "app") != 0) return false;
    if (!pet_p4_miniapp_dispatch_input(
          miniapp_event,
          ts_ms,
          action_override,
          action_override_size
        )) return false;
    state->last_update_ms = ts_ms;
    return true;
  }
  return false;
}

static bool component_system_action(const char *action) {
  return action
    && (
      strcmp(action, "page_toggle") == 0
      || strcmp(action, "session_next") == 0
      || strcmp(action, "session_previous") == 0
      || strcmp(action, "page_enter") == 0
      || strcmp(action, "page_back") == 0
      || strcmp(action, "page_main") == 0
      || strcmp(action, "page_app") == 0
      || strcmp(action, "component_center") == 0
    );
}

static const pet_p4_input_binding_t *active_global_priority_binding(
  const pet_p4_runtime_state_t *state,
  const char *event_name
) {
  if (!state) return NULL;
  const pet_p4_input_binding_t *binding = active_binding(event_name);
  // Realtime is global, including component lists. It must win over a package
  // action even when the user remaps it away from the factory SW2 long press.
  if (binding && strcmp(binding->action, "realtime_chat") == 0) return binding;
  return binding && strcmp(state->screen_page, "app") == 0
    && strcmp(binding->action, "page_back") == 0 ? binding : NULL;
}

static bool dispatch_component_binding_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  pet_p4_input_binding_t binding = {0};
  char component_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  char action_override[PET_P4_MINIAPP_ACTION_MAX] = {0};
  bool handled_locally;
  if (!state || strcmp(state->screen_page, "app") != 0) return false;
  if (!pet_p4_miniapp_resolve_input(
        event_name,
        component_action,
        sizeof(component_action)
      )) return false;

  // Component packages own gameplay actions only. Older packages may still
  // contain page_main/page_back records; ignore those records so the current
  // persisted global exit mapping remains the single navigation authority.
  if (component_system_action(component_action)) return false;

  copy_text(binding.event, sizeof(binding.event), event_name);
  copy_text(binding.action, sizeof(binding.action), "miniapp_action");
  copy_text(binding.value, sizeof(binding.value), component_action);
  handled_locally = apply_local_action(
    state,
    &binding,
    event->ts_ms,
    action_override,
    sizeof(action_override)
  );
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    &binding,
    action_override,
    handled_locally
  );
  return true;
}

static void send_ignored_component_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  pet_p4_input_binding_t ignored_binding = {0};
  copy_text(ignored_binding.event, sizeof(ignored_binding.event), event_name);
  copy_text(ignored_binding.action, sizeof(ignored_binding.action), "disabled");
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    &ignored_binding,
    "",
    true
  );
}

static void dispatch_binding_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  const pet_p4_input_binding_t *binding = active_binding(event_name);
  if (pet_p4_conversation_active(state)) {
    // The realtime mode owns input: never move/confirm/send into an Agent session.
    if (binding && (!strcmp(binding->action, "realtime_chat") || !strcmp(binding->action, "page_back"))) {
      pet_p4_input_binding_t exit = *binding;
      copy_text(exit.action, sizeof(exit.action), "realtime_chat");
      send_input_event(state, send_line, ctx, event, event_name, gesture, &exit, "", false);
      return;
    }
    if (!strcmp(event_name, "joystick.up") || !strcmp(event_name, "knob.rotate_ccw"))
      pet_p4_conversation_move(state, -1, event->ts_ms);
    else if (!strcmp(event_name, "joystick.down") || !strcmp(event_name, "knob.rotate_cw"))
      pet_p4_conversation_move(state, 1, event->ts_ms);
    send_ignored_component_event(state, send_line, ctx, event, event_name, gesture);
    return;
  }
  // Leaving a component keeps music playing. A further configured Back on
  // the pet home page stops it; realtime mode above retains input priority.
  if (state && binding && strcmp(state->screen_page, "main") == 0
      && strcmp(binding->action, "page_back") == 0) {
    pet_p4_media_stop_background(send_line, ctx);
    state->last_update_ms = event->ts_ms;
    send_input_event(state, send_line, ctx, event, event_name, gesture, binding, "media_stop", true);
    return;
  }
  const pet_p4_input_binding_t *global_exit = active_global_priority_binding(state, event_name);
  if (global_exit) {
    char miniapp_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
    const bool handled_locally = apply_local_action(
      state,
      global_exit,
      event->ts_ms,
      miniapp_action,
      sizeof(miniapp_action)
    );
    send_input_event(
      state,
      send_line,
      ctx,
      event,
      event_name,
      gesture,
      global_exit,
      miniapp_action,
      handled_locally
    );
    return;
  }
  if (dispatch_component_binding_event(
        state,
        send_line,
        ctx,
        event,
        event_name,
        gesture
      )) return;
  // While a component is open, its package owns optional gameplay buttons.
  // An unmapped system-navigation gesture must not fall through to the global
  // action (for example SW2 -> component_center) and look like a second exit.
  // The configured page_back and realtime_chat gestures were handled above.
  if (state
      && strcmp(state->screen_page, "app") == 0
      && binding
      && component_system_action(binding->action)) {
    send_ignored_component_event(
      state,
      send_line,
      ctx,
      event,
      event_name,
      gesture
    );
    return;
  }
  char miniapp_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  bool handled_locally = apply_local_action(
    state,
    binding,
    event->ts_ms,
    miniapp_action,
    sizeof(miniapp_action)
  );
  if (binding && strcmp(binding->action, "voice_ptt") == 0) {
    if (strcmp(gesture, "hold_end") == 0) {
      if (state) state->session_voice_active = false;
      handled_locally = pet_p4_audio_capture_stop() == ESP_OK;
    } else if (!handled_locally && strcmp(gesture, "hold_start") == 0) {
      if (state) state->session_voice_active = true;
      handled_locally = pet_p4_audio_capture_start(state && state->session_queue_count == 0) == ESP_OK;
    }
  }
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    binding,
    miniapp_action,
    handled_locally
  );
}

static void process_button_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event
) {
  char event_name[PET_P4_INPUT_EVENT_MAX];
  const char *prefix = button_event_prefix(event->control);
  if (!prefix[0]) return;

  if (event->gesture == PET_P4_INPUT_GESTURE_HOLD_START
      || event->gesture == PET_P4_INPUT_GESTURE_HOLD_END) {
    char long_event_name[PET_P4_INPUT_EVENT_MAX];
    snprintf(long_event_name, sizeof(long_event_name), "%s.long_press", prefix);
    if (state && !pet_p4_conversation_active(state)
        && strcmp(state->screen_page, "app") == 0
        && pet_p4_miniapp_has_input(long_event_name)) {
      return;
    }
    snprintf(event_name, sizeof(event_name), "%s.hold", prefix);
    const pet_p4_input_binding_t *binding = active_binding(event_name);
    if (binding && strcmp(binding->action, "disabled") != 0) {
      dispatch_binding_event(
        state,
        send_line,
        ctx,
        event,
        event_name,
        event->gesture == PET_P4_INPUT_GESTURE_HOLD_START ? "hold_start" : "hold_end"
      );
    }
    return;
  }
  if (event->gesture == PET_P4_INPUT_GESTURE_SHORT_PRESS) {
    snprintf(event_name, sizeof(event_name), "%s.short_press", prefix);
    dispatch_binding_event(state, send_line, ctx, event, event_name, "short_press");
  } else if (event->gesture == PET_P4_INPUT_GESTURE_LONG_PRESS) {
    snprintf(event_name, sizeof(event_name), "%s.long_press", prefix);
    dispatch_binding_event(state, send_line, ctx, event, event_name, "long_press");
  }
}

void pet_p4_input_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  pet_p4_input_event_t event;
  if (!g_event_queue) return;
  // The engine task only stores raw levels; debounce/long-press decoding runs
  // here on the main tick, replacing the removed 5 ms sampling task.
  advance_button_decoders();
  while (xQueueReceive(g_event_queue, &event, 0) == pdTRUE) {
    if (event.control == PET_P4_INPUT_CONTROL_JOYSTICK
        && event.gesture == PET_P4_INPUT_GESTURE_DIRECTION) {
      const char *event_name = "";
      const char *gesture = "";
      switch ((pet_p4_joystick_direction_t) event.delta) {
        case PET_P4_JOYSTICK_UP:
          event_name = "joystick.up";
          gesture = "up";
          event.delta = 0;
          break;
        case PET_P4_JOYSTICK_DOWN:
          event_name = "joystick.down";
          gesture = "down";
          event.delta = 0;
          break;
        case PET_P4_JOYSTICK_LEFT:
          event_name = "knob.rotate_ccw";
          gesture = "left";
          event.delta = -1;
          break;
        case PET_P4_JOYSTICK_RIGHT:
          event_name = "knob.rotate_cw";
          gesture = "right";
          event.delta = 1;
          break;
        default:
          break;
      }
      if (!event_name[0]) continue;
      if (state && !pet_p4_conversation_active(state)
          && strcmp(state->screen_page, "app") == 0
          && !pet_p4_miniapp_has_input(event_name)
          && !active_global_priority_binding(state, event_name)) {
        send_ignored_component_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          gesture
        );
        continue;
      }
      dispatch_binding_event(state, send_line, ctx, &event, event_name, gesture);
    } else if (event.control == PET_P4_INPUT_CONTROL_ENCODER
        && event.gesture == PET_P4_INPUT_GESTURE_ROTATE) {
      const char *event_name = event.delta > 0 ? "knob.rotate_cw" : "knob.rotate_ccw";
      if (state && !pet_p4_conversation_active(state) && strcmp(state->screen_page, "components") == 0
          && !active_global_priority_binding(state, event_name)) {
        static const pet_p4_input_binding_t select_binding = {
          .event = "knob.rotate_cw",
          .action = "component_select",
          .value = "",
        };
        (void) pet_p4_miniapp_catalog_move(event.delta);
        state->last_update_ms = event.ts_ms;
        send_input_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          event.delta > 0 ? "rotate_cw" : "rotate_ccw",
          &select_binding,
          "component_select",
          true
        );
        continue;
      }
      if (state && !pet_p4_conversation_active(state)
          && strcmp(state->screen_page, "app") == 0
          && !pet_p4_miniapp_has_input(event_name)
          && !active_global_priority_binding(state, event_name)) {
        send_ignored_component_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          event.delta > 0 ? "rotate_cw" : "rotate_ccw"
        );
        continue;
      }
      dispatch_binding_event(
        state,
        send_line,
        ctx,
        &event,
        event_name,
        event.delta > 0 ? "rotate_cw" : "rotate_ccw"
      );
    } else {
      process_button_event(state, send_line, ctx, &event);
    }
  }
}

unsigned int pet_p4_input_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}

void pet_p4_input_get_joystick_snapshot(pet_p4_input_joystick_snapshot_t *snapshot) {
  // The ESP-Mosaico board has no analog joystick: directions arrive as
  // injected touch-swipe pulses. The snapshot stays in the public contract
  // (diagnostics/PC client) but is permanently disarmed.
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
}
