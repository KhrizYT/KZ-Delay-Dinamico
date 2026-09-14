/*
 * Broadcast Delay - low-level GPU drawing helpers (see gfx-util.hpp).
 */
#include "gfx-util.hpp"

#include <graphics/vec4.h>
#include <cstdio>
#include <cstring>

void draw_texture(gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, cx, cy);
}

gs_effect_t *opacity_effect()
{
	static gs_effect_t *eff = nullptr;
	if (eff)
		return eff;
	const char *src =
		"uniform float4x4 ViewProj;\n"
		"uniform texture2d image;\n"
		"uniform float opacity;\n"
		"sampler_state ss { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
		"struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
		"VI VS(VI v) { VI o; o.pos = mul(float4(v.pos.xyz,1.0), ViewProj); o.uv = v.uv; return o; }\n"
		"float4 PS(VI v) : TARGET { float4 c = image.Sample(ss, v.uv); c.a *= opacity; return c; }\n"
		"technique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";
	eff = gs_effect_create(src, nullptr, nullptr);
	return eff;
}

void draw_blend(gs_texture_t *a, gs_texture_t *b, float frac, uint32_t cx,
		uint32_t cy)
{
	draw_texture(a, cx, cy); /* opaque base */
	if (!b || frac <= 0.001f)
		return;
	gs_effect_t *eff = opacity_effect();
	if (!eff) {
		if (frac >= 0.5f)
			draw_texture(b, cx, cy);
		return;
	}
	gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), b);
	gs_effect_set_float(gs_effect_get_param_by_name(eff, "opacity"), frac);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(b, 0, cx, cy);
	gs_blend_state_pop();
}

void draw_rect(float x, float y, float w, float h, float r, float g, float b,
	       float a)
{
	if (w <= 0.0f || h <= 0.0f)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	vec4_set(&c, r, g, b, a);
	gs_effect_set_vec4(color, &c);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
}

void draw_digit(float x, float y, float w, float h, int d, float r, float g,
		float b)
{
	static const unsigned char seg[10] = {
		/* a b c d e f g */
		0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
	if (d < 0 || d > 9)
		return;
	const unsigned char m = seg[d];
	const float t = w * 0.18f; /* segment thickness */
	const float hh = h * 0.5f;
	if (m & 0x01) /* a top */
		draw_rect(x, y, w, t, r, g, b, 1.0f);
	if (m & 0x40) /* g middle */
		draw_rect(x, y + hh - t * 0.5f, w, t, r, g, b, 1.0f);
	if (m & 0x08) /* d bottom */
		draw_rect(x, y + h - t, w, t, r, g, b, 1.0f);
	if (m & 0x20) /* f top-left */
		draw_rect(x, y, t, hh, r, g, b, 1.0f);
	if (m & 0x02) /* b top-right */
		draw_rect(x + w - t, y, t, hh, r, g, b, 1.0f);
	if (m & 0x10) /* e bottom-left */
		draw_rect(x, y + hh, t, hh, r, g, b, 1.0f);
	if (m & 0x04) /* c bottom-right */
		draw_rect(x + w - t, y + hh, t, hh, r, g, b, 1.0f);
}

void draw_number(uint32_t canvas_cx, float y, float dh, int value, float cr,
		 float cg, float cb)
{
	if (value < 0)
		value = 0;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	const int n = (int)strlen(buf);
	const float dw = dh * 0.6f;
	const float gap = dh * 0.25f;
	const float total = n * dw + (n - 1) * gap;
	float x = ((float)canvas_cx - total) * 0.5f;
	for (int i = 0; i < n; i++) {
		draw_digit(x, y, dw, dh, buf[i] - '0', cr, cg, cb);
		x += dw + gap;
	}
}

void draw_number_neg(gs_texrender_t *scratch, uint32_t cx, uint32_t cy, float y,
		     float dh, int value)
{
	if (!scratch)
		return;
	gs_texrender_reset(scratch);
	if (gs_texrender_begin(scratch, cx, cy)) {
		struct vec4 clr;
		vec4_zero(&clr);
		gs_clear(GS_CLEAR_COLOR, &clr, 0.f, 0);
		gs_ortho(0.f, (float)cx, 0.f, (float)cy, -100.f, 100.f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO); /* opaque white */
		draw_number(cx, y, dh, value, 1.f, 1.f, 1.f);
		gs_blend_state_pop();
		gs_texrender_end(scratch);
	}
	gs_texture_t *tex = gs_texrender_get_texture(scratch);
	if (!tex)
		return;
	gs_ortho(0.f, (float)cx, 0.f, (float)cy, -100.f, 100.f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_INVDSTCOLOR, GS_BLEND_INVSRCALPHA,
				   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	draw_texture(tex, cx, cy);
	gs_blend_state_pop();
}

void upload_frame_texture(gs_texture_t *tex, const uint8_t *data, uint32_t cx,
			  uint32_t cy)
{
	if (!tex || !data || !cx || !cy)
		return;
	const uint32_t row = cx * 4u;
	uint8_t *ptr = nullptr;
	uint32_t pitch = 0;
	if (!gs_texture_map(tex, &ptr, &pitch))
		return;
	for (uint32_t y = 0; y < cy; y++) {
		memcpy(ptr + (size_t)y * pitch, data + (size_t)y * row, row);
		if (pitch > row)
			memset(ptr + (size_t)y * pitch + row, 0, pitch - row);
	}
	gs_texture_unmap(tex);
}
