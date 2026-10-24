/*
 * Host-simulator stubs for the device-only audio backend (pet_p4_audio.c is
 * ESP-IDF only). Music streaming and instrument playback report inert state
 * so media/instrument pages render their idle layouts.
 */
#include <stdio.h>

#include "pet_p4_platform.h"
#include "pet_p4_audio.h"

bool pet_p4_audio_instrument_strike(uint64_t now_ms) {
  (void) now_ms;
  return false;
}

pet_instrument_motion_t pet_p4_audio_instrument_motion(uint64_t now_ms) {
  (void) now_ms;
  pet_instrument_motion_t idle = {0};
  return idle;
}

bool pet_p4_audio_music_active(void) { return false; }
bool pet_p4_audio_music_paused(void) { return false; }
bool pet_p4_audio_music_ended(void) { return true; }
void pet_p4_audio_music_pause(bool paused) { (void) paused; }
void pet_p4_audio_music_volume(unsigned int volume) { (void) volume; }
unsigned int pet_p4_audio_music_buffered_ms(void) { return 0; }
esp_err_t pet_p4_audio_music_begin(const char *session_id) {
  (void) session_id;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pet_p4_audio_stream_push(const uint8_t *pcm, size_t length) {
  (void) pcm; (void) length;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pet_p4_audio_stream_end(void) { return ESP_ERR_NOT_SUPPORTED; }
void pet_p4_audio_stream_flush(void) {}
unsigned int pet_p4_audio_stream_played_bytes(void) { return 0; }
bool pet_p4_audio_stream_playing(void) { return false; }

/* ---- protocol state helpers (pet_p4_protocol.c is device-only) ---- */

#include <string.h>
#include "pet_p4_protocol.h"

void pet_p4_state_init(pet_p4_runtime_state_t *state, const char *board_device_id) {
  memset(state, 0, sizeof(*state));
  snprintf(state->board_device_id, sizeof(state->board_device_id), "%s",
           board_device_id ? board_device_id : "sim");
}

bool pet_p4_state_touch_feedback_active(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  (void) state; (void) now_ms;
  return false;
}

bool pet_p4_state_host_connected(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  (void) state; (void) now_ms;
  return true;
}

/* ---- system cue WAVs (embedded binaries exist only in the device build) ---- */

/* Symbol names must match the device build's EMBED_FILES labels. */
const uint8_t _binary_done_wav_start[1] = {0};
const uint8_t _binary_done_wav_end[1] = {0};
const uint8_t _binary_error_wav_start[1] = {0};
const uint8_t _binary_error_wav_end[1] = {0};
const uint8_t _binary_waiting_user_wav_start[1] = {0};
const uint8_t _binary_waiting_user_wav_end[1] = {0};
