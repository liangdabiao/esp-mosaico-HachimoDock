/*
 * Base64 include wrapper: mbedtls on device, host implementation in
 * pet_host_shims.c for the simulator build. The host shim exposes the same
 * mbedtls_base64_* signatures.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MOSAICO_HOST_SIMULATION
int pet_host_base64_encode(
  unsigned char *dst, size_t dlen, size_t *olen,
  const unsigned char *src, size_t slen
);
int pet_host_base64_decode(
  unsigned char *dst, size_t dlen, size_t *olen,
  const unsigned char *src, size_t slen
);
#define mbedtls_base64_encode pet_host_base64_encode
#define mbedtls_base64_decode pet_host_base64_decode
#else
#include "mbedtls/base64.h"
#endif

#ifdef __cplusplus
}
#endif
