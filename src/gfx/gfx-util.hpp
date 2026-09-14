/*
 * Broadcast Delay - low-level GPU drawing helpers.
 *
 * Pure, stateless graphics primitives (no DelayedSource / engine state): texture
 * blits, solid rects, a 7-segment number renderer and a packed-RGBA upload. The
 * render path builds on these, so they live in their own reusable module.
 *
 * All functions must run on the OBS graphics thread (inside a render pass).
 */
#pragma once

#include <obs-module.h>
#include <cstdint>

/* Blit a texture over a cx*cy sprite using the default effect. */
void draw_texture(gs_texture_t *tex, uint32_t cx, uint32_t cy);

/* Lazily-built effect that draws a texture with a global opacity (for blends). */
gs_effect_t *opacity_effect();

/* Crossfade between two textures by `frac` (0..1) -> smooth slow/fast motion. */
void draw_blend(gs_texture_t *a, gs_texture_t *b, float frac, uint32_t cx,
		uint32_t cy);

/* Filled rectangle in the current coordinate space. */
void draw_rect(float x, float y, float w, float h, float r, float g, float b,
	       float a);

/* One 7-segment digit at (x,y) sized w x h. */
void draw_digit(float x, float y, float w, float h, int d, float r, float g,
		float b);

/* Draw a non-negative integer centred at canvas_cx (top y) with digit height dh. */
void draw_number(uint32_t canvas_cx, float y, float dh, int value, float cr,
		 float cg, float cb);

/* Like draw_number but the inversion effect is applied ONCE: digits are rendered
 * white into `scratch` first (overlapping 7-segment corners merge) then
 * composited with the negative blend. `scratch` is a caller-owned texrender. */
void draw_number_neg(gs_texrender_t *scratch, uint32_t cx, uint32_t cy, float y,
		     float dh, int value);

/* Upload tightly packed RGBA rows into a dynamic texture (pads GPU row pitch). */
void upload_frame_texture(gs_texture_t *tex, const uint8_t *data, uint32_t cx,
			  uint32_t cy);
