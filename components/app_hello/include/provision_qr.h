// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True means a black QR module. Coordinates are module indexes, not pixels. */
typedef bool (*provision_module_fn)(int x, int y, void *ctx);

/*
 * Encode a square module grid as an opaque RGB QOI image.
 * A quiet zone of `quiet` modules surrounds the grid. Each module is `scale`
 * pixels. out == NULL returns the byte count a later call must provide.
 * Returns 0 when the arguments or the buffer cannot hold the image.
 */
size_t provision_qoi_encode(int modules, provision_module_fn module, void *ctx,
                            int quiet, int scale, uint8_t *out, size_t cap);

/*
 * Encode text as a QR image, quiet zone 4, largest scale that fits in 280 px.
 * ECC is low and the version stays in 1..6. out == NULL returns the size.
 */
size_t provision_qr_qoi(const char *text, uint8_t *out, size_t cap);
