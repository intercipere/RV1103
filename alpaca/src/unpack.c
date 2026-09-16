#include "unpack.h"
#include <stddef.h>

void unpack_bits_lsb(const uint8_t *packed, int stride_bytes, int width,
                      int height, uint16_t *out) {
	for (int y = 0; y < height; y++) {
		const uint8_t *row = packed + (size_t)y * stride_bytes;
		uint16_t *orow = out + (size_t)y * width;
		int x = 0;

		/* Fast path: 4 pixels per 5 bytes, byte-aligned groups. */
		for (; x + 4 <= width; x += 4) {
			const uint8_t *g = row + (x / 4) * 5;
			orow[x + 0] = (uint16_t)(g[0] | ((g[1] & 0x03) << 8));
			orow[x + 1] = (uint16_t)((g[1] >> 2) | ((g[2] & 0x0F) << 6));
			orow[x + 2] = (uint16_t)((g[2] >> 4) | ((g[3] & 0x3F) << 4));
			orow[x + 3] = (uint16_t)((g[3] >> 6) | (g[4] << 2));
		}

		/* Generic bit-by-bit fallback for a width not divisible by 4
		 * (kept for reuse with other sensors -- current SC3336/IMX290
		 * widths are all divisible by 4, so this loop is never hit today). */
		for (; x < width; x++) {
			int bitpos = x * 10;
			uint16_t v = 0;
			for (int b = 0; b < 10; b++) {
				int k = bitpos + b;
				int byte_idx = k / 8, bit_idx = k % 8;
				int bit = (row[byte_idx] >> bit_idx) & 1;
				v = (uint16_t)(v | (bit << b));
			}
			orow[x] = v;
		}
	}
}

void unpack_bits_lsb_transposed(const uint8_t *packed, int stride_bytes,
                                 int width, int height, uint16_t *out) {
#define UT_TILE 32
	for (int y0 = 0; y0 < height; y0 += UT_TILE) {
		int yn = (y0 + UT_TILE <= height) ? UT_TILE : height - y0;
		for (int x0 = 0; x0 < width; x0 += UT_TILE) {
			int xn = (x0 + UT_TILE <= width) ? UT_TILE : width - x0;
			for (int dy = 0; dy < yn; dy++) {
				int y = y0 + dy;
				const uint8_t *row = packed + (size_t)y * stride_bytes;
				/* x0 steps by UT_TILE (a multiple of 4), so the 4-pixel/
				 * 5-byte groups are always aligned at a tile boundary. */
				const uint8_t *g = row + (size_t)(x0 / 4) * 5;
				int dx = 0;
				for (; dx + 4 <= xn; dx += 4, g += 5) {
					int x = x0 + dx;
					out[(size_t)(x + 0) * height + y] =
					    (uint16_t)(g[0] | ((g[1] & 0x03) << 8));
					out[(size_t)(x + 1) * height + y] =
					    (uint16_t)((g[1] >> 2) | ((g[2] & 0x0F) << 6));
					out[(size_t)(x + 2) * height + y] =
					    (uint16_t)((g[2] >> 4) | ((g[3] & 0x3F) << 4));
					out[(size_t)(x + 3) * height + y] =
					    (uint16_t)((g[3] >> 6) | (g[4] << 2));
				}
				/* Ragged right edge of a tile (width not a multiple of 4);
				 * never hit at current SC3336/IMX290 widths. */
				for (; dx < xn; dx++) {
					int x = x0 + dx;
					int bitpos = x * 10;
					uint16_t v = 0;
					for (int b = 0; b < 10; b++) {
						int k = bitpos + b;
						v = (uint16_t)(v | (((row[k / 8] >> (k % 8)) & 1) << b));
					}
					out[(size_t)x * height + y] = v;
				}
			}
		}
	}
#undef UT_TILE
}
