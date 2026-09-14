#include "delay/png-stb.hpp"

#include <cstring>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb/stb_image_write.h"

bool png_stb_write(const char *path, int width, int height,
		   const uint8_t *rgba, int stride_bytes, int compression)
{
	if (!path || !rgba || width <= 0 || height <= 0)
		return false;
	int level = compression;
	if (level < 0)
		level = 0;
	if (level > 9)
		level = 9;
	stbi_write_png_compression_level = level;
	return stbi_write_png(path, width, height, 4, rgba, stride_bytes) != 0;
}

bool png_stb_read(const char *path, int width, int height, uint8_t *rgba_out,
		  size_t rgba_bytes)
{
	if (!path || !rgba_out || width <= 0 || height <= 0)
		return false;
	int w = 0, h = 0, comp = 0;
	unsigned char *px =
		stbi_load(path, &w, &h, &comp, 4);
	if (!px)
		return false;
	const size_t need = (size_t)width * (size_t)height * 4u;
	bool ok = (w == width && h == height && rgba_bytes >= need);
	if (ok)
		memcpy(rgba_out, px, need);
	stbi_image_free(px);
	return ok;
}
