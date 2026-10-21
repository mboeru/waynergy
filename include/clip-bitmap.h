#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A validated view of the uncompressed 24/32-bit DIB used by the protocol.
 * Both canonical INFOHEADER and extended/masked Windows DIBs are accepted. */
struct clipBitmap {
	const unsigned char *pixels;
	uint32_t width, height;
	size_t stride, pixel_offset;
	unsigned bits_per_pixel;
	bool top_down, repaired_legacy_header;
	uint32_t masks[4]; /* red, green, blue, alpha (zero means opaque) */
};

uint32_t clipBitmapLe32(const unsigned char *p);
void clipBitmapWriteLe32(unsigned char *p, uint32_t value);
bool clipBitmapParse(const unsigned char *data, size_t len, struct clipBitmap *bitmap);
/* Hash visible RGB values, independent of padding, orientation, masks and alpha.
 * Outgoing protocol images are 24-bit, so alpha does not affect echo matching. */
bool clipBitmapHash(const unsigned char *data, size_t len, uint64_t *result);
/* Return a newly allocated canonical BMP file for the PNG converter. Explicit
 * alpha masks are retained; unused BI_RGB high bytes are treated as opaque. */
bool clipBitmapToBmp(const unsigned char *data, size_t len,
	unsigned char **bmp, size_t *bmp_len);
