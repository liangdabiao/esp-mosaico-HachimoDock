/*
 * [Input] Microphone/speaker PCM frames from pet_p4_audio.c.
 * [Output] Inert pass-through: v1 of the Mosaico port ships without the
 *          ESP-SR AEC front end, so capture runs half-duplex exactly like
 *          HachimoDock's aec_fallback path (hello reports audioAec=false).
 * [Pos] Swap-in point for the esp-sr front end (AFE supports esp32s31).
 */
#include "pet_p4_audio_frontend.h"

bool pet_p4_afe_init(void) { return false; }
bool pet_p4_afe_ready(void) { return false; }
void pet_p4_afe_reset_stream(void) {}
bool pet_p4_afe_feed(const int16_t *mic) {
  (void) mic;
  return false;
}
bool pet_p4_afe_take(int16_t *pcm, bool *speech) {
  (void) pcm;
  if (speech) *speech = false;
  return false;
}
void pet_p4_afe_reference(const int16_t *pcm, size_t samples) {
  (void) pcm;
  (void) samples;
}
void pet_p4_afe_reference_abort(void) {}
