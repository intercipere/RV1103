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
