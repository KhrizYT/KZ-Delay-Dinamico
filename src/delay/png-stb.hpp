#pragma once

#include <cstddef>
#include <cstdint>

/* Lossless PNG via stb (RGBA in/out, same layout as RAW disk buffer). */
bool png_stb_write(const char *path, int width, int height,
		   const uint8_t *rgba, int stride_bytes, int compression);
bool png_stb_read(const char *path, int width, int height, uint8_t *rgba_out,
		  size_t rgba_bytes);
