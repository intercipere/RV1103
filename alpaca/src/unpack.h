#ifndef UNPACK_H
#define UNPACK_H

#include <stdint.h>

/*
 * Compact RAW10: 4 pixels packed into 5 bytes, little-endian bit order
 * within each byte AND little-endian bit assembly within each pixel's 10
 * bits (bit 0 of the pixel is the first bit encountered in the byte
 * stream). This is the packing validated for the SC3336/IMX290 family on
 * this platform -- see repo CLAUDE.md, "Validated sensor/driver facts".
 * Getting the bit order wrong produces pixel-scrambled noise that looks
 * identical across all four Bayer phases, not a shifted/rotated image.
 */
void unpack_bits_lsb(const uint8_t *packed, int stride_bytes, int width,
                      int height, uint16_t *out);

#endif
