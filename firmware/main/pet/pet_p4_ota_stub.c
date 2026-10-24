/*
 * [Input] OTA topic dispatch from the shared protocol core.
 * [Output] Fail-closed "unsupported" answers plus inert health hooks.
 * [Pos] ESP-Mosaico replaces HachimoDock's in-app USB OTA writer with the
 *       retained Recovery workflow: all firmware writes go through
 *       `python mosaico.py iris system-update/app-update` (see
 *       esp_mosaico_app_recovery / iris_ota_support_start). This stub keeps
 *       the protocol surface intact while refusing device writes.
 */
#include "pet_p4_ota.h"

#include <string.h>

#include "pet_p4_platform.h"

esp_err_t pet_p4_ota_init(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  (void) state;
  (void) send_line;
  (void) ctx;
  return ESP_OK;
}

bool pet_p4_ota_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!topic || strncmp(topic, "firmware/", strlen("firmware/")) != 0) {
    return false;
  }
  (void) payload;
  if (send_line) {
    send_line(
      "{\"topic\":\"protocol/ack\",\"payload\":{\"ok\":false,"
      "\"error\":\"firmware_update_unsupported\","
      "\"reason\":\"ESP-Mosaico firmware updates use mosaico.py iris "
      "system-update; the application never writes its own image\"}}",
      ctx
    );
  }
  return true;
}

void pet_p4_ota_runtime_ready(unsigned long long now_ms) { (void) now_ms; }
void pet_p4_ota_runtime_failed(unsigned long long now_ms) { (void) now_ms; }
void pet_p4_ota_process(unsigned long long now_ms) { (void) now_ms; }

void pet_p4_ota_send_status(pet_p4_send_line_fn send_line, void *ctx) {
  if (!send_line) return;
  send_line(
    "{\"topic\":\"firmware/status\",\"payload\":{\"supported\":false,"
    "\"writer\":\"mosaico-recovery\",\"active\":false}}",
    ctx
  );
}

bool pet_p4_ota_active(void) { return false; }
bool pet_p4_ota_transfer_active(void) { return false; }
