#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

static inline uint32_t clipBitmapLe32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compare image content across BMP header versions, row orientations and
 * 24/32-bit conversions. Ignore padding and alpha, which BMP3 conversion drops.
 * This keeps our incoming wl-copy offers from becoming outgoing clipboard grabs. */
static inline bool clipBitmapHash(const unsigned char *data, size_t len, uint64_t *result)
{
	if (len < 40)
		return false;
	uint32_t header = clipBitmapLe32(data);
	int32_t width = (int32_t)clipBitmapLe32(data + 4);
	int32_t height = (int32_t)clipBitmapLe32(data + 8);
	unsigned bpp = (unsigned)data[14] | ((unsigned)data[15] << 8);
	uint32_t compression = clipBitmapLe32(data + 16);
	if ((header != 40 && header != 108 && header != 124) ||
	    width <= 0 || !height || height == INT32_MIN ||
	    data[12] != 1 || data[13] != 0 || (bpp != 24 && bpp != 32) ||
	    (compression != 0 && compression != 3))
		return false;
	uint64_t rows = height < 0 ? -(int64_t)height : height;
	uint64_t stride = (((uint64_t)width * bpp + 31) / 32) * 4;
	uint64_t bytes = stride * rows;
	if (bytes > len - 40)
		return false;
	/* Older macOS senders truncate V4/V5 headers to 40 bytes. */
	if (header > 40 && (uint64_t)header + bytes > len)
		header = 40;
	if ((uint64_t)header + bytes > len)
		return false;
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned i = 0; i < 4; ++i) {
		hash = (hash ^ ((uint32_t)width >> (i * 8) & 255)) * UINT64_C(1099511628211);
		hash = (hash ^ (rows >> (i * 8) & 255)) * UINT64_C(1099511628211);
	}
	for (uint64_t y = 0; y < rows; ++y) {
		const unsigned char *row = data + header + (height > 0 ? rows - y - 1 : y) * stride;
		for (int32_t x = 0; x < width; ++x)
			for (unsigned channel = 0; channel < 3; ++channel)
				hash = (hash ^ row[(size_t)x * (bpp / 8) + channel]) * UINT64_C(1099511628211);
	}
	*result = hash;
	return true;
}
