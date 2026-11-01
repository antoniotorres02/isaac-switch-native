/* QR codes for the sign-in screen: byte mode, error correction level M,
 * versions 1-10, which is far more than Steam's challenge URL needs. */
#ifndef ISAAC_NATIVE_QR_H
#define ISAAC_NATIVE_QR_H

#include <stddef.h>
#include <stdint.h>

/* Modules per side of the largest code (version 10). */
#define ISAAC_QR_MAX_SIZE 57

/* Encodes `len` bytes into `modules`, row by row, one byte per module (1 is
 * dark); it must hold ISAAC_QR_MAX_SIZE squared. `mask` is 0-7, or -1 to pick
 * the one the standard's penalty rules prefer. Returns the modules per side,
 * 0 when the data does not fit. */
int isaac_qr_encode(const void *data, size_t len, int mask, uint8_t *modules);

#endif
