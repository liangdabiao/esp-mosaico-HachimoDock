/*
 * cJSON include wrapper: ESP-IDF component on device, vendored copy for the
 * host simulator build (whose include path cannot be extended). Keep all pet
 * sources including this instead of "cJSON.h" directly.
 */
#pragma once

#ifdef MOSAICO_HOST_SIMULATION
#include "host_only/cJSON.h"
#else
#include "cJSON.h"
#endif
