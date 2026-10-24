/*
 * [Input] Physical-input runtime lifecycle, engine-injected hardware events,
 *         and desktop input-config commands.
 * [Output] Version-9 input configuration API with 16 persisted bindings plus
 *          raw-level injection for the board buttons and touch-derived
 *          joystick directions; the joystick snapshot remains as a disarmed
 *          stub for the diagnostics contract (no analog joystick here).
 * [Pos] Public interface for the ESP-Mosaico input runtime.
 * [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.
 */

#pragma once

#include <stdbool.h>

#include "pet_p4_cjson.h"
#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_INPUT_CONFIG_VERSION 9
#define PET_P4_INPUT_MAX_BINDINGS 16
#define PET_P4_INPUT_EVENT_MAX 40
#define PET_P4_INPUT_ACTION_MAX 24
#define PET_P4_INPUT_VALUE_MAX 160

typedef struct {
  bool ready;
  int center_x;
  int center_y;
  int current_x;
  int current_y;
  int minimum_x;
  int maximum_x;
  int minimum_y;
  int maximum_y;
} pet_p4_input_joystick_snapshot_t;

/* Injected hardware controls: the AI button feeds SW1, the Boot button feeds
 * SW2, and the shared center key doubles as the encoder press (the historical
 * board routed both through one key). Touch swipes feed the four joystick
 * direction controls; no rotary encoder or analog joystick exists here. */
typedef enum {
  PET_P4_INPUT_CONTROL_SW1 = 0,
  PET_P4_INPUT_CONTROL_SW2,
  PET_P4_INPUT_CONTROL_SW3,
  PET_P4_INPUT_CONTROL_ENCODER_PRESS,
  PET_P4_INPUT_CONTROL_ENCODER,
  PET_P4_INPUT_CONTROL_JOYSTICK,
  PET_P4_INPUT_CONTROL_JOYSTICK_UP,
  PET_P4_INPUT_CONTROL_JOYSTICK_DOWN,
  PET_P4_INPUT_CONTROL_JOYSTICK_LEFT,
  PET_P4_INPUT_CONTROL_JOYSTICK_RIGHT,
} pet_p4_input_control_t;

esp_err_t pet_p4_input_init(void);
/* Feed one raw hardware level from the engine task (task context, not ISR):
 * button controls take the current pressed state, joystick direction controls
 * emit one completed direction pulse when pressed is true. Gestures are
 * decoded later in pet_p4_input_process() on the main tick. */
void pet_p4_input_hw_event(int control, bool pressed);
esp_err_t pet_p4_input_reset_config(void);
void pet_p4_input_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_input_handle_config(
  const cJSON *payload,
  bool legacy_button_config,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_input_send_config_state(
  const cJSON *request,
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
);
unsigned int pet_p4_input_dropped_events(void);
void pet_p4_input_get_joystick_snapshot(pet_p4_input_joystick_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
