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

/*
 * Same packing, but writes the result already transposed into ASCOM
 * ImageBytes wire order -- out[x * height + y], X outer -- instead of
 * row-major. This exists because the daemon's only consumer of the frame is
 * the wire format: unpacking row-major and transposing afterwards costs a
 * full extra write + read pass over ~6MB (measured 67ms unpack + 76ms pack
 * on RV1103), whereas doing it here is one pass. Blocked into 32x32 tiles so
 * both the strided packed reads and the strided output writes touch only ~32
 * cache lines at a time; the naive version of this is catastrophically
 * cache-hostile (every write a miss, 3M of them).
 */
void unpack_bits_lsb_transposed(const uint8_t *packed, int stride_bytes,
                                 int width, int height, uint16_t *out);

#endif
