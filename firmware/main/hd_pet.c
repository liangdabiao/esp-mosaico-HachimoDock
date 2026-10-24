/*
 * [Input] Engine callbacks (mosaico_game_app), CDC protocol traffic, device
 *         input events, and the shared HachimoDock runtime modules in pet/.
 * [Output] The complete pet runtime: serialized state access, protocol worker,
 *          per-tick state processing and frame presentation on the engine
 *          canvas.
 * [Pos] ESP-Mosaico runtime coordinator replacing HachimoDock pet_p4_main.c.
 *       Transports collapse to the application-owned USB CDC link (hd_cdc);
 *       display/input/touch/audio backends live in the engine/BSP/adapted
 *       pet modules. ESP-Iris runs on TCP (sdkconfig.defaults) and stays
 *       reachable for mosaico.py operations.
 * [Sync] USB-ownership exception is documented in the project README.
 */
#include "hd_pet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mosaico_game_2d.h"
#include "mosaico_raylib_fast.h"

#include "hd_cdc.h"
#include "hd_storage.h"

#include "pet/pet_p4_audio.h"
#include "pet/pet_p4_diagnostics.h"
#include "pet/pet_p4_input.h"
#include "pet/pet_p4_miniapp.h"
#include "pet/pet_p4_ota.h"
#include "pet/pet_p4_protocol.h"
#include "pet/pet_p4_renderer.h"
#include "pet/pet_p4_touch.h"
#include "pet/pet_p4_view.h"

static const char *TAG = "hd_pet";

#define HD_PET_PROTOCOL_TASK_STACK_BYTES 12288
#define HD_PET_PROTOCOL_QUEUE_DEPTH 24
#define HD_PET_PROTOCOL_LINE_BUFFER_BYTES 32768
#define HD_PET_HELLO_RETRY_MS 2000ULL
#define HD_PET_SCREENSHOT_W 320
#define HD_PET_SCREENSHOT_H 240
#define HD_PET_SCREENSHOT_RAW_CHUNK 3072

static pet_p4_runtime_state_t g_state;
static char *g_line_buffer;
static size_t g_line_used;
static bool g_raw_mode;
static SemaphoreHandle_t g_render_mutex;
static SemaphoreHandle_t g_state_mutex;
static QueueHandle_t g_protocol_queue;
static pet_p4_runtime_state_t *g_render_state;
static bool s_runtime_ready;
static bool s_transfer_was_active;
static unsigned long long s_last_hello_ms;

typedef struct {
  char *line;
} hd_pet_protocol_request_t;

/* ---- transport: single CDC route ---- */

static void transport_send_line(const char *line, void *ctx) {
  (void) ctx;
  if (!line) return;
  size_t len = strlen(line);
  hd_cdc_write_all(line, len);
  hd_cdc_write_all("\n", 1);
}

/* ---- protocol worker ---- */

static bool enqueue_protocol_line(const char *line) {
  if (!line || !line[0] || !g_protocol_queue) return false;
  size_t line_len = strlen(line);
  char *line_copy = (char *) heap_caps_malloc(
    line_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!line_copy) line_copy = (char *) malloc(line_len + 1);
  if (!line_copy) return false;
  memcpy(line_copy, line, line_len + 1);
  hd_pet_protocol_request_t request = { .line = line_copy };
  if (xQueueSend(g_protocol_queue, &request, pdMS_TO_TICKS(20)) == pdTRUE) {
    return true;
  }
  free(line_copy);
  transport_send_line(
    "{\"topic\":\"protocol/error\",\"payload\":{\"ok\":false,\"error\":\"command_queue_busy\"}}",
    NULL);
  return false;
}

static bool consume_byte(char ch) {
  if (ch == '\r') return false;
  if (ch == '\n') {
    g_line_buffer[g_line_used] = '\0';
    if (g_line_used > 0) {
      (void) enqueue_protocol_line(g_line_buffer);
    }
    g_line_used = 0;
    return pet_p4_raw_asset_chunk_active();
  }
  if (g_line_used + 1 < HD_PET_PROTOCOL_LINE_BUFFER_BYTES) {
    g_line_buffer[g_line_used++] = ch;
  } else {
    g_line_used = 0;
  }
  return false;
}

static void hd_cdc_rx_handler(const uint8_t *data, size_t len, void *ctx) {
  (void) ctx;
  size_t offset = 0;
  while (offset < len) {
    if (!g_raw_mode && pet_p4_raw_asset_chunk_active()) g_raw_mode = true;
    if (g_raw_mode) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      size_t consumed = pet_p4_consume_raw_asset_bytes(
        data + offset, len - offset, transport_send_line, NULL);
      xSemaphoreGive(g_state_mutex);
      if (consumed > 0) {
        offset += consumed;
      }
      if (!pet_p4_raw_asset_chunk_active() || consumed == 0) {
        g_raw_mode = false;
      }
      continue;
    }
    if (consume_byte((char) data[offset])) {
      g_raw_mode = true;
    }
    offset += 1;
  }
}

static void maybe_send_screenshot(const char *line) {
  if (!line || !strstr(line, "\"debug/screenshot\"")) return;

  uint8_t *raw = (uint8_t *) malloc(HD_PET_SCREENSHOT_W * HD_PET_SCREENSHOT_H * 2);
  if (!raw) {
    transport_send_line(
      "{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"no memory\"}}", NULL);
    return;
  }
  if (!g_render_mutex
      || xSemaphoreTake(g_render_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    free(raw);
    transport_send_line(
      "{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"renderer busy\"}}",
      NULL);
    return;
  }
  int src_w = 0;
  int src_h = 0;
  const uint16_t *src = pet_p4_renderer_logical_framebuffer(&src_w, &src_h);
  if (!src || src_w <= 0 || src_h <= 0) {
    xSemaphoreGive(g_render_mutex);
    free(raw);
    transport_send_line(
      "{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"no framebuffer\"}}",
      NULL);
    return;
  }
  unsigned int non_black = 0;
  for (int y = 0; y < HD_PET_SCREENSHOT_H; y += 1) {
    int sy = y * src_h / HD_PET_SCREENSHOT_H;
    for (int x = 0; x < HD_PET_SCREENSHOT_W; x += 1) {
      int sx = x * src_w / HD_PET_SCREENSHOT_W;
      uint16_t px = src[sy * src_w + sx];
      if (px != 0) non_black += 1;
      size_t off = (size_t) (y * HD_PET_SCREENSHOT_W + x) * 2;
      raw[off] = (uint8_t) (px & 0xff);
      raw[off + 1] = (uint8_t) (px >> 8);
    }
  }
  xSemaphoreGive(g_render_mutex);

  char id[32];
  char header[256];
  snprintf(id, sizeof(id), "%llu",
           (unsigned long long) (esp_timer_get_time() / 1000ULL));
  snprintf(header, sizeof(header),
           "{\"topic\":\"debug/screenshot_begin\",\"payload\":{\"id\":\"%s\","
           "\"width\":%d,\"height\":%d,\"format\":\"rgb565le\"}}",
           id, HD_PET_SCREENSHOT_W, HD_PET_SCREENSHOT_H);
  transport_send_line(header, NULL);

  /* 3/4 base64 expansion of 3072 raw bytes plus JSON wrapper stays below the
   * protocol's per-line limit. */
  static char chunk[4 * ((HD_PET_SCREENSHOT_RAW_CHUNK + 2) / 3) + 192];
  size_t total = HD_PET_SCREENSHOT_W * HD_PET_SCREENSHOT_H * 2;
  int index = 0;
  for (size_t offset = 0; offset < total; offset += HD_PET_SCREENSHOT_RAW_CHUNK) {
    size_t n = total - offset;
    if (n > HD_PET_SCREENSHOT_RAW_CHUNK) n = HD_PET_SCREENSHOT_RAW_CHUNK;
    static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    size_t prefix = (size_t) snprintf(
      chunk, sizeof(chunk),
      "{\"topic\":\"debug/screenshot_chunk\",\"payload\":{\"id\":\"%s\",\"index\":%d,\"data\":\"",
      id, index);
    out = prefix;
    for (size_t i = 0; i < n; i += 3) {
      unsigned v = (unsigned) raw[offset + i] << 16;
      if (i + 1 < n) v |= (unsigned) raw[offset + i + 1] << 8;
      if (i + 2 < n) v |= (unsigned) raw[offset + i + 2];
      chunk[out++] = b64[(v >> 18) & 0x3f];
      chunk[out++] = b64[(v >> 12) & 0x3f];
      chunk[out++] = (i + 1 < n) ? b64[(v >> 6) & 0x3f] : '=';
      chunk[out++] = (i + 2 < n) ? b64[v & 0x3f] : '=';
    }
    chunk[out++] = '"';
    chunk[out++] = '}';
    chunk[out++] = '}';
    chunk[out] = '\0';
    hd_cdc_write_all(chunk, out);
    hd_cdc_write_all("\n", 1);
    index += 1;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  char end[160];
  snprintf(end, sizeof(end),
           "{\"topic\":\"debug/screenshot_end\",\"payload\":{\"id\":\"%s\",\"chunks\":%d,"
           "\"nonBlack\":%u}}",
           id, index, non_black);
  transport_send_line(end, NULL);
  free(raw);
}

static void protocol_worker_task(void *arg) {
  (void) arg;
  hd_pet_protocol_request_t request;
  while (true) {
    if (xQueueReceive(g_protocol_queue, &request, portMAX_DELAY) != pdTRUE) continue;
    if (strstr(request.line, "\"debug/screenshot\"")) {
      maybe_send_screenshot(request.line);
    } else {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      pet_p4_handle_line(&g_state, request.line, transport_send_line, NULL);
      xSemaphoreGive(g_state_mutex);
    }
    free(request.line);
  }
}

/* ---- lifecycle from main.c ---- */

static void build_board_device_id(char *out, size_t out_size) {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
    snprintf(out, out_size, "m31-unknown");
    return;
  }
  snprintf(out, out_size, "m31-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t hd_pet_start(void) {
  char board_device_id[PET_P4_DEVICE_ID_MAX];

  ESP_ERROR_CHECK(nvs_flash_init());
  esp_err_t storage_err = hd_storage_mount();
  if (storage_err != ESP_OK) {
    ESP_LOGE(TAG, "/nand mount failed; widget/appearance storage degraded: %s",
             esp_err_to_name(storage_err));
  }

  g_line_buffer = heap_caps_calloc(
    1, HD_PET_PROTOCOL_LINE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_RETURN_ON_FALSE(g_line_buffer != NULL, ESP_ERR_NO_MEM, TAG, "line buffer");
  g_render_mutex = xSemaphoreCreateMutex();
  g_state_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(
    g_render_mutex && g_state_mutex, ESP_ERR_NO_MEM, TAG, "mutexes");

  esp_err_t diagnostics_err = pet_p4_diagnostics_init();
  if (diagnostics_err != ESP_OK) {
    ESP_LOGW(TAG, "diagnostics persistence unavailable: %s",
             esp_err_to_name(diagnostics_err));
  }

  build_board_device_id(board_device_id, sizeof(board_device_id));
  pet_p4_state_init(&g_state, board_device_id);
  g_render_state = (pet_p4_runtime_state_t *) heap_caps_calloc(
    1, sizeof(*g_render_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_render_state) g_render_state = calloc(1, sizeof(*g_render_state));
  ESP_RETURN_ON_FALSE(g_render_state != NULL, ESP_ERR_NO_MEM, TAG, "render state");

  g_protocol_queue = xQueueCreate(
    HD_PET_PROTOCOL_QUEUE_DEPTH, sizeof(hd_pet_protocol_request_t));
  ESP_RETURN_ON_FALSE(g_protocol_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue");
  ESP_RETURN_ON_FALSE(
    xTaskCreate(protocol_worker_task, "hd_pet_protocol",
                HD_PET_PROTOCOL_TASK_STACK_BYTES, NULL, 11, NULL) == pdPASS,
    ESP_ERR_NO_MEM, TAG, "protocol worker");

  esp_err_t miniapp_err = pet_p4_miniapp_init();
  if (miniapp_err != ESP_OK && miniapp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mini-app init failed: %s", esp_err_to_name(miniapp_err));
  }
  esp_err_t builtin_sync_err = pet_p4_miniapp_sync_builtins();
  if (builtin_sync_err != ESP_OK) {
    ESP_LOGW(TAG, "built-in component sync failed: %s",
             esp_err_to_name(builtin_sync_err));
  }
  /* Parse the large appearance manifest before CDC traffic can race it. */
  pet_p4_load_asset_manifest(&g_state);

  ESP_RETURN_ON_ERROR(pet_p4_input_init(), TAG, "input init");
  esp_err_t touch_err = pet_p4_touch_init();
  if (touch_err != ESP_OK) {
    ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(touch_err));
  }
  ESP_RETURN_ON_ERROR(pet_p4_renderer_init(), TAG, "renderer init");

  esp_err_t audio_err = pet_p4_audio_init(
    board_device_id, transport_send_line, NULL);
  if (audio_err != ESP_OK) {
    ESP_LOGW(TAG, "microphone unavailable, keeping non-voice controls: %s",
             esp_err_to_name(audio_err));
  }
  esp_err_t ota_err = pet_p4_ota_init(&g_state, transport_send_line, NULL);
  if (ota_err != ESP_OK) {
    ESP_LOGW(TAG, "OTA support init failed: %s", esp_err_to_name(ota_err));
  }

  ESP_RETURN_ON_ERROR(hd_cdc_init(hd_cdc_rx_handler, NULL), TAG, "cdc init");

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  pet_p4_send_hello(&g_state, transport_send_line, NULL);
  xSemaphoreGive(g_state_mutex);
  pet_p4_audio_send_status();

  s_runtime_ready = true;
  ESP_LOGI(TAG, "HachimoDock Mosaico pet runtime ready (CDC pet link)");
  return ESP_OK;
}

void hd_pet_tick(void) {
  if (!s_runtime_ready) return;
  unsigned long long now_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
  bool firmware_transfer_active = false;

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  pet_p4_diagnostics_process(now_ms, &g_state);
  pet_p4_ota_process(now_ms);
  pet_p4_asset_transfer_process(&g_state, now_ms);
  pet_p4_miniapp_process(now_ms);
  pet_p4_state_process(&g_state, now_ms);
  pet_p4_touch_process(&g_state, transport_send_line, NULL);
  pet_p4_input_process(&g_state, transport_send_line, NULL);
  uint8_t instrument_effect = 0, instrument_ambience = 0;
  bool instrument_open = !strcmp(g_state.screen_page, "app")
    && pet_p4_miniapp_instrument_config(&instrument_effect, &instrument_ambience);
  pet_p4_audio_instrument_set(
    instrument_open && !g_state.asset_transfer_active
      && !pet_p4_ota_transfer_active() && !pet_p4_audio_conversation_active(),
    instrument_effect, instrument_ambience);
  pet_p4_audio_process(&g_state, now_ms);
  firmware_transfer_active = pet_p4_ota_transfer_active();
  bool transfer_active =
    g_state.asset_transfer_active || firmware_transfer_active;
  if (s_transfer_was_active && !transfer_active) {
    pet_p4_renderer_reset_playback();
  }
  s_transfer_was_active = transfer_active;
  if (!g_state.desktop_device_id[0]
      && now_ms - s_last_hello_ms > HD_PET_HELLO_RETRY_MS) {
    pet_p4_send_hello(&g_state, transport_send_line, NULL);
    s_last_hello_ms = now_ms;
  }
  xSemaphoreGive(g_state_mutex);
}

void hd_pet_render(void) {
  if (!s_runtime_ready) return;
  unsigned long long now_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
  bool firmware_transfer = false;
  pet_p4_view_model_t view = {0};

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  bool transfer_active =
    g_state.asset_transfer_active || pet_p4_ota_transfer_active();
  firmware_transfer = pet_p4_ota_transfer_active();
  if (!transfer_active) {
    memcpy(g_render_state, &g_state, sizeof(*g_render_state));
    pet_p4_build_view_model(g_render_state, &view);
  }
  xSemaphoreGive(g_state_mutex);

  BeginDrawing();
  if (!MosaicoFastFrameAvailable()) {
    EndDrawing();
    return;
  }
  xSemaphoreTake(g_render_mutex, portMAX_DELAY);
  esp_err_t err = transfer_active
    ? pet_p4_renderer_render_transfer_status(firmware_transfer, now_ms)
    : pet_p4_renderer_render(g_render_state, &view, now_ms);
  if (err == ESP_OK) {
    int width = 0;
    int height = 0;
    const uint16_t *pixels =
      pet_p4_renderer_logical_framebuffer(&width, &height);
    if (pixels) {
      /* Zero-copy borrow: the renderer-owned framebuffer is stable for the
       * application lifetime, so register once and blit every frame. */
      static Texture2D s_frame;
      if (s_frame.id == 0) {
        s_frame = Mosaico2DRegisterRGB565(pixels, width, height);
      }
      if (s_frame.id != 0) {
        MosaicoFastDrawTexture(s_frame, 0, 0, WHITE);
      }
    }
  }
  xSemaphoreGive(g_render_mutex);
  EndDrawing();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "render failed: %s", esp_err_to_name(err));
  }
}

void hd_pet_pointer_event(int32_t x, int32_t y, bool pressed) {
  unsigned long long now_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
  pet_p4_touch_hw_sample((int) x, (int) y, pressed, now_ms);
}

void hd_pet_button_event(int32_t button, bool pressed) {
  /* AI (GPIO7) is SW1, Boot (GPIO61) is SW2 in the HachimoDock gesture map. */
  pet_p4_input_hw_event(
    button == 0 ? PET_P4_INPUT_CONTROL_SW1 : PET_P4_INPUT_CONTROL_SW2, pressed);
}

void hd_pet_touch_swipe_as_joystick(int dx, int dy) {
  int control = -1;
  if (dy < 0) control = PET_P4_INPUT_CONTROL_JOYSTICK_UP;
  else if (dy > 0) control = PET_P4_INPUT_CONTROL_JOYSTICK_DOWN;
  else if (dx < 0) control = PET_P4_INPUT_CONTROL_JOYSTICK_LEFT;
  else if (dx > 0) control = PET_P4_INPUT_CONTROL_JOYSTICK_RIGHT;
  if (control >= 0) {
    pet_p4_input_hw_event(control, true);
    pet_p4_input_hw_event(control, false);
  }
}
