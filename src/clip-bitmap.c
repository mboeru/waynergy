#include "clip-bitmap.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

uint32_t clipBitmapLe32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void clipBitmapWriteLe32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		p[i] = (value >> (8 * i)) & 255;
}

static bool contiguousMask(uint32_t mask)
{
	if (!mask)
		return false;
	while (!(mask & 1))
		mask >>= 1;
	return !(mask & (mask + 1));
}

/* Older macOS converters drop the V5 extension but leave biSize=124 and
 * BI_BITFIELDS. Only repair the exact INFOHEADER + BGRA payload shape. A
 * generally truncated extended header must remain an error. */
static bool legacyMacHeader(uint32_t header, unsigned bpp, uint32_t compression,
	uint64_t pixels, size_t len)
{
	return header == 124 && bpp == 32 && compression == 3 && pixels == len - 40;
}

bool clipBitmapParse(const unsigned char *data, size_t len, struct clipBitmap *out)
{
	if (!data || !out || len < 40)
		return false;
	uint32_t header = clipBitmapLe32(data);
	int32_t width = (int32_t)clipBitmapLe32(data + 4);
	int32_t signed_height = (int32_t)clipBitmapLe32(data + 8);
	unsigned bpp = (unsigned)data[14] | ((unsigned)data[15] << 8);
	uint32_t compression = clipBitmapLe32(data + 16);
	uint32_t colors = clipBitmapLe32(data + 32);
	if ((header != 40 && header != 52 && header != 56 && header != 108 && header != 124) ||
	    width <= 0 || !signed_height || signed_height == INT32_MIN ||
	    data[12] != 1 || data[13] != 0 || (bpp != 24 && bpp != 32) ||
	    (compression != 0 && compression != 3 && compression != 6) ||
	    (compression != 0 && bpp != 32))
		return false;
	uint32_t height = signed_height < 0 ? -(int64_t)signed_height : signed_height;
	uint64_t stride = (((uint64_t)width * bpp + 31) / 32) * 4;
	uint64_t pixel_bytes = stride * height;
	bool repair = !colors && legacyMacHeader(header, bpp, compression, pixel_bytes, len);
	uint64_t offset = repair ? 40 : header;
	uint32_t masks[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0};
	if (!repair && compression) {
		if (header == 40)
			offset += compression == 6 ? 16 : 12;
		if (offset > len || (compression == 6 && header != 40 && header < 56))
			return false;
		for (unsigned i = 0; i < 3; ++i)
			masks[i] = clipBitmapLe32(data + 40 + 4 * i);
		if (header >= 56 || compression == 6)
			masks[3] = clipBitmapLe32(data + 52);
		uint32_t used = 0;
		for (unsigned i = 0; i < 4; ++i) {
			if (i == 3 && !masks[i] && compression != 6)
				continue;
			if (!contiguousMask(masks[i]) || (used & masks[i]))
				return false;
			used |= masks[i];
		}
	}
	/* True-color DIBs can still carry an optimization color table. */
	offset += (uint64_t)colors * 4;
	if (offset > len || pixel_bytes > len - offset || stride > SIZE_MAX)
		return false;
	*out = (struct clipBitmap){
		.pixels = data + (size_t)offset,
		.width = width, .height = height, .stride = stride, .pixel_offset = offset,
		.bits_per_pixel = bpp, .top_down = signed_height < 0,
		.repaired_legacy_header = repair,
	};
	memcpy(out->masks, masks, sizeof(masks));
	return true;
}

static unsigned char channel(uint32_t pixel, uint32_t mask)
{
	if (!mask)
		return 255;
	while (!(mask & 1)) {
		pixel >>= 1;
		mask >>= 1;
	}
	return ((uint64_t)(pixel & mask) * 255 + mask / 2) / mask;
}

static void pixelBgra(const struct clipBitmap *bitmap, uint32_t x, uint32_t y,
	unsigned char color[4])
{
	const unsigned char *p = bitmap->pixels +
		(bitmap->top_down ? y : bitmap->height - y - 1) * bitmap->stride +
		(size_t)x * (bitmap->bits_per_pixel / 8);
	if (bitmap->masks[0] == 0x00ff0000 && bitmap->masks[1] == 0x0000ff00 &&
	    bitmap->masks[2] == 0x000000ff) {
		memcpy(color, p, 3);
		color[3] = !bitmap->masks[3] ? 255 : bitmap->masks[3] == 0xff000000 ? p[3] :
			channel(clipBitmapLe32(p), bitmap->masks[3]);
		return;
	}
	uint32_t pixel = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
	if (bitmap->bits_per_pixel == 32)
		pixel |= (uint32_t)p[3] << 24;
	color[0] = channel(pixel, bitmap->masks[2]);
	color[1] = channel(pixel, bitmap->masks[1]);
	color[2] = channel(pixel, bitmap->masks[0]);
	color[3] = channel(pixel, bitmap->masks[3]);
}

bool clipBitmapHash(const unsigned char *data, size_t len, uint64_t *result)
{
	struct clipBitmap bitmap;
	if (!result || !clipBitmapParse(data, len, &bitmap))
		return false;
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned i = 0; i < 4; ++i) {
		hash = (hash ^ (bitmap.width >> (i * 8) & 255)) * UINT64_C(1099511628211);
		hash = (hash ^ (bitmap.height >> (i * 8) & 255)) * UINT64_C(1099511628211);
	}
	for (uint32_t y = 0; y < bitmap.height; ++y)
		for (uint32_t x = 0; x < bitmap.width; ++x) {
			unsigned char color[4];
			pixelBgra(&bitmap, x, y, color);
			for (unsigned c = 0; c < 3; ++c)
				hash = (hash ^ color[c]) * UINT64_C(1099511628211);
		}
	*result = hash;
	return true;
}

bool clipBitmapToBmp(const unsigned char *data, size_t len,
	unsigned char **bmp, size_t *bmp_len)
{
	struct clipBitmap bitmap;
	if (!bmp || !bmp_len || !clipBitmapParse(data, len, &bitmap))
		return false;
	bool alpha = bitmap.masks[3] != 0;
	size_t header = alpha ? 108 : 40;
	uint64_t stride = ((uint64_t)bitmap.width * (alpha ? 4 : 3) + 3) & ~UINT64_C(3);
	uint64_t total = 14 + header + stride * bitmap.height;
	if (total > UINT32_MAX || total > SIZE_MAX)
		return false;
	unsigned char *output = calloc(1, total);
	if (!output)
		return false;
	output[0] = 'B'; output[1] = 'M';
	clipBitmapWriteLe32(output + 2, total);
	clipBitmapWriteLe32(output + 10, 14 + header);
	unsigned char *dib = output + 14;
	clipBitmapWriteLe32(dib, header);
	clipBitmapWriteLe32(dib + 4, bitmap.width);
	clipBitmapWriteLe32(dib + 8, bitmap.height);
	dib[12] = 1; dib[14] = alpha ? 32 : 24;
	clipBitmapWriteLe32(dib + 20, stride * bitmap.height);
	if (alpha) {
		clipBitmapWriteLe32(dib + 16, 3);
		for (unsigned i = 0; i < 4; ++i)
			clipBitmapWriteLe32(dib + 40 + 4 * i,
				(uint32_t[]){0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000}[i]);
		clipBitmapWriteLe32(dib + 56, 0x73524742); /* LCS_sRGB */
	}
	for (uint32_t y = 0; y < bitmap.height; ++y)
		for (uint32_t x = 0; x < bitmap.width; ++x) {
			unsigned char color[4];
			pixelBgra(&bitmap, x, y, color);
			memcpy(dib + header + (bitmap.height - y - 1) * stride + x * (alpha ? 4u : 3u),
				color, alpha ? 4 : 3);
		}
	*bmp = output;
	*bmp_len = total;
	return true;
}
