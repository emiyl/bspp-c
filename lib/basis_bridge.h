#ifndef BSPP_BASIS_BRIDGE_H
#define BSPP_BASIS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool bspp_encode_pvrtc2(const uint32_t *argb, uint32_t width, uint32_t height,
                       uint8_t *blocks, size_t size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
