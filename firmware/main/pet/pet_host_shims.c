/*
 * Host-simulator shims behind pet_p4_platform.h / pet_p4_base64.h.
 * Compiled only in the MOSAICO_HOST_SIMULATION build (see game.sim.json).
 */
#include "pet_p4_platform.h"
#include "pet_p4_base64.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
/* GetTickCount64 keeps the simulator DLL free of libwinpthread-1.dll, which
 * is not on PATH outside the WinLibs tree (clock_gettime lives there). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
unsigned long long pet_host_time_ms(void) {
  return (unsigned long long) GetTickCount64();
}
#else
#include <time.h>
unsigned long long pet_host_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long) ts.tv_sec * 1000ULL
    + (unsigned long long) ts.tv_nsec / 1000000ULL;
}
#endif

void *pet_host_malloc(size_t size) { return malloc(size); }

void *pet_host_calloc(size_t count, size_t size) { return calloc(count, size); }

void pet_host_free(void *ptr) { free(ptr); }

/* Minimal canonical base64, mbedtls-compatible return codes (0 on success). */

static const char base64_chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int pet_host_base64_encode(
  unsigned char *dst, size_t dlen, size_t *olen,
  const unsigned char *src, size_t slen
) {
  if (!olen) return -1;
  size_t need = ((slen + 2) / 3) * 4;
  *olen = 0;
  if (!dst || dlen < need) return -1;
  size_t out = 0;
  for (size_t i = 0; i < slen; i += 3) {
    unsigned v = (unsigned) src[i] << 16;
    if (i + 1 < slen) v |= (unsigned) src[i + 1] << 8;
    if (i + 2 < slen) v |= (unsigned) src[i + 2];
    dst[out++] = (unsigned char) base64_chars[(v >> 18) & 0x3f];
    dst[out++] = (unsigned char) base64_chars[(v >> 12) & 0x3f];
    dst[out++] = (i + 1 < slen)
      ? (unsigned char) base64_chars[(v >> 6) & 0x3f] : '=';
    dst[out++] = (i + 2 < slen)
      ? (unsigned char) base64_chars[v & 0x3f] : '=';
  }
  *olen = out;
  return 0;
}

static int base64_value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

int pet_host_base64_decode(
  unsigned char *dst, size_t dlen, size_t *olen,
  const unsigned char *src, size_t slen
) {
  if (!olen) return -1;
  *olen = 0;
  if (slen % 4 != 0) return -1;
  size_t out = 0;
  for (size_t i = 0; i < slen; i += 4) {
    int v[4];
    size_t keep = 3;
    for (size_t k = 0; k < 4; ++k) {
      char ch = (char) src[i + k];
      if (ch == '=') {
        if (k < 2) return -1;
        keep = k - 1;
        v[k] = 0;
      } else {
        if (k > 0 && src[i + k - 1] == '=') return -1;
        v[k] = base64_value(ch);
        if (v[k] < 0) return -1;
      }
    }
    if (!dst || out + keep > dlen) return -1;
    unsigned trip = (unsigned) (v[0] << 18) | ((unsigned) v[1] << 12)
      | ((unsigned) v[2] << 6) | (unsigned) v[3];
    if (keep >= 1) dst[out++] = (unsigned char) (trip >> 16);
    if (keep >= 2) dst[out++] = (unsigned char) (trip >> 8);
    if (keep >= 3) dst[out++] = (unsigned char) trip;
  }
  *olen = out;
  return 0;
}
