/*
 * Broadcast Delay - source video render (see dse-render.hpp).
 */
#include "dse-render.hpp"

#include "../core/dse-internal.hpp"
#include "core/dse-constants.hpp"
#include "../gfx/gfx-util.hpp"
#include "warp/warp-control.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

/* Snapshot the frame shown just before a Live<->Delay jump, so it can be faded
 * out over the OBS transition duration. Called at the top of the render, before
 * the new (post-jump) frame overwrites upload_tex/last_vram. */
static void warp_trans_snapshot(DelayedSource *s, uint32_t cx, uint32_t cy)
{
	if (!s->warp_trans_arm.exchange(false, std::memory_order_relaxed))
		return;
	gs_texture_t *cur = s->has_uploaded ? s->upload_tex : s->last_vram;
	if (!cur) {
		s->warp_trans_start.store(0, std::memory_order_relaxed);
		return;
	}
	if (!s->warp_from_tex || s->warp_from_w != cx || s->warp_from_h != cy) {
		if (s->warp_from_tex)
			gs_texture_destroy(s->warp_from_tex);
		s->warp_from_tex = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr,
						     GS_RENDER_TARGET);
		s->warp_from_w = cx;
		s->warp_from_h = cy;
	}
	if (!s->warp_from_tex) {
		s->warp_trans_start.store(0, std::memory_order_relaxed);
		return;
	}
	gs_copy_texture(s->warp_from_tex, cur);
	s->warp_trans_start.store((int64_t)os_gettime_ns(),
				  std::memory_order_relaxed);
}

/* Crossfade progress (0..1) while a Live<->Delay transition is active, else -1.
 * Auto-finishes when the OBS transition duration elapses. */
static float warp_trans_progress(DelayedSource *s)
{
	const int64_t start = s->warp_trans_start.load(std::memory_order_relaxed);
	if (start == 0 || !s->warp_from_tex)
		return -1.f;
	const int64_t dur = s->warp_trans_dur.load(std::memory_order_relaxed);
	const int64_t el = (int64_t)os_gettime_ns() - start;
	if (dur <= 0 || el >= dur) {
		s->warp_trans_start.store(0, std::memory_order_relaxed);
		return -1.f;
	}
	return (float)el / (float)dur;
}


/* Dark panel behind the countdown number. The number is drawn with an inversion
 * blend (bright on black), so on a HOLD SCENE it would otherwise be unreadable;
 * this darkens the area under it. On a black hold it is invisible (black/black).
 * `numY` is the number's top (= the y passed to draw_number_neg). */
static void draw_countdown_backdrop(uint32_t cx, uint32_t cy, float numY, float dh)
{
	const float bw = cy * 0.5f;
	const float bh = dh + cy * 0.12f;
	draw_rect((cx - bw) * 0.5f, numY - cy * 0.06f, bw, bh, 0.f, 0.f, 0.f, 0.55f);
}

/* Paused: hold scene (or black) + the countdown / progress overlay. */
static void render_paused(DelayedSource *s, uint32_t cx, uint32_t cy, int64_t ph)
{
	std::string hold;
	{
		std::lock_guard<std::mutex> lock(g_dock_mutex);
		hold = g_pause_scene;
	}
	bool drew_scene = false;
	if (!hold.empty()) {
		obs_source_t *sc = obs_get_source_by_name(hold.c_str());
		if (sc) {
			obs_source_video_render(sc);
			obs_source_release(sc);
			drew_scene = true;
		}
	}
	if (!drew_scene)
		draw_rect(0.0f, 0.0f, (float)cx, (float)cy,
			  0.0f, 0.0f, 0.0f, 1.0f); /* black */

	/* Countdown + progress bar drawn on top, with a dark
	 * semi-transparent backdrop for readability on any scene. */
	if (s->show_countdown.load(std::memory_order_relaxed)) {
		const int64_t now = (int64_t)os_gettime_ns();
		int64_t target =
			(int64_t)s->delay_ns.load(std::memory_order_relaxed);
		const int64_t buf =
			s->buffered_ns.load(std::memory_order_relaxed);
		if (buf > 0 && target > buf)
			target = buf;
		double prog = target > 0 ? (double)(now - ph) / (double)target
					 : 1.0;
		if (prog < 0.0)
			prog = 0.0;
		if (prog > 1.0)
			prog = 1.0;
		const float dh = cy * 0.18f;
		const float bar_h = cy * 0.028f;
		const double remain = (double)(target - (now - ph)) / 1.0e9;
		const int secs = remain > 0 ? (int)(remain + 0.999) : 0;
		/* Negative effect applied once (offscreen) so segment
		 * corners aren't inverted twice. */
		draw_countdown_backdrop(cx, cy, cy * 0.36f, dh);
		draw_number_neg(s->blur_texrender, cx, cy, cy * 0.36f, dh, secs);
		const float bw = cx * 0.5f;
		const float bx = (cx - bw) * 0.5f, by = cy * 0.60f;
		draw_rect(bx - bar_h * 0.2f, by - bar_h * 0.2f,
			  bw + bar_h * 0.4f, bar_h + bar_h * 0.4f,
			  0.0f, 0.0f, 0.0f, 0.30f); /* track bg */
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_INVDSTCOLOR, GS_BLEND_ZERO,
					   GS_BLEND_ONE, GS_BLEND_ZERO);
		draw_rect(bx, by, bw * (float)prog, bar_h, 1.0f, 1.0f, 1.0f,
			  1.0f); /* fill */
		gs_blend_state_pop();
	}
}

/* Buffer not yet filled to the target delay: hold scene / black + a fill
 * countdown. Returns true if it drew the countdown (caller then stops). */
static bool render_fill_countdown(DelayedSource *s, uint32_t cx, uint32_t cy)
{
	if (!(s->show_countdown.load(std::memory_order_relaxed) &&
	      s->warp_state.load(std::memory_order_relaxed) == WARP_DELAYED))
		return false;
	const double fps = active_fps();
	const int64_t delay_ns =
		(int64_t)s->delay_ns.load(std::memory_order_relaxed);
	/* Real fill = frames actually captured so far (reset to 0 when the
	 * buffer is created), vs the frames needed for the target delay. */
	const size_t required =
		(size_t)std::ceil((double)delay_ns / 1.0e9 * fps);
	const size_t buf = s->frames_buffered.load(std::memory_order_relaxed);
	if (!(required > 1 && buf < required))
		return false;
	const double fill_sec = (double)buf / (fps > 0 ? fps : 60.0);
	const double target_sec = (double)delay_ns / 1.0e9;
	const double remain = target_sec - fill_sec;
	const int secs = remain > 0 ? (int)(remain + 0.999) : 0;
	double prog = target_sec > 0 ? fill_sec / target_sec : 1.0;
	if (prog < 0.0)
		prog = 0.0;
	if (prog > 1.0)
		prog = 1.0;
	/* Background: the pause hold scene if one is set (so it's not a black
	 * screen while the buffer fills), else black. */
	std::string hold;
	{
		std::lock_guard<std::mutex> lock(g_dock_mutex);
		hold = g_pause_scene;
	}
	bool drew_scene = false;
	if (!hold.empty()) {
		obs_source_t *sc = obs_get_source_by_name(hold.c_str());
		if (sc) {
			obs_source_video_render(sc);
			obs_source_release(sc);
			drew_scene = true;
		}
	}
	if (!drew_scene)
		draw_rect(0.0f, 0.0f, (float)cx, (float)cy, 0.0f, 0.0f, 0.0f,
			  1.0f); /* black */
	const float dh = cy * 0.18f;
	draw_countdown_backdrop(cx, cy, cy * 0.36f, dh);
	draw_number_neg(s->blur_texrender, cx, cy, cy * 0.36f, dh, secs);
	/* Fill progress bar */
	const float bar_h = cy * 0.028f;
	const float bw = cx * 0.5f;
	const float bx = (cx - bw) * 0.5f, by = cy * 0.60f;
	draw_rect(bx - bar_h * 0.2f, by - bar_h * 0.2f, bw + bar_h * 0.4f,
		  bar_h + bar_h * 0.4f, 0.0f, 0.0f, 0.0f, 0.30f); /* track bg */
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_INVDSTCOLOR, GS_BLEND_ZERO,
				   GS_BLEND_ONE, GS_BLEND_ZERO);
	draw_rect(bx, by, bw * (float)prog, bar_h, 1.0f, 1.0f, 1.0f,
		  1.0f); /* fill */
	gs_blend_state_pop();
	return true;
}


void dse_video_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<DelayedSource *>(data);

	if (!s->enabled.load(std::memory_order_relaxed))
		return; /* disabled -> draw nothing */

	/* If we are being rendered *inside* our own capture pass (we are part of
	 * the scene/program we delay), draw nothing to avoid an infinite
	 * delayed-inside-delayed feedback. */
	if (s->rendering.load(std::memory_order_relaxed))
		return;

	const uint32_t cx = s->cx.load(std::memory_order_relaxed);
	const uint32_t cy = s->cy.load(std::memory_order_relaxed);
	if (cx == 0 || cy == 0)
		return;

	/* If a Live<->Delay jump was just requested, freeze the current frame
	 * for the crossfade before it gets overwritten below. */
	warp_trans_snapshot(s, cx, cy);

	const int storage = s->storage.load(std::memory_order_relaxed);
	const bool disk_mode = (storage == STORAGE_DISK);

	/* The playhead (set by the time-warp tick) is the capture-time to show. */
	const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
	const uint64_t target_time = ph > 0 ? (uint64_t)ph : 0;
	bool ram_blend = false;
	float ram_frac = 0.0f;

	/* Paused: hold scene / black + countdown. */
	if (s->warp_state.load(std::memory_order_relaxed) == WARP_PAUSED) {
		render_paused(s, cx, cy, ph);
		return;
	}

	/* Buffer still filling: show the fill countdown. */
	if (render_fill_countdown(s, cx, cy))
		return;

	/* Interpolate (crossfade) only during non-1x motion. At 1x the playhead
	 * advances at the capture rate, so single-frame sampling is already
	 * smooth -- and avoids ghosting/cost in the common stable case. */
	const int wstate = s->warp_state.load(std::memory_order_relaxed);
	const bool interp = (wstate == WARP_ACCEL || wstate == WARP_DECEL);

	const float tprog = warp_trans_progress(s);
	const bool xfade = tprog >= 0.f && s->warp_from_tex;

	if (storage == STORAGE_VRAM) {
		gs_texture_t *a = nullptr, *b = nullptr;
		float frac = 0.0f;
		if (s->vram.ready() && s->vram.sample2(target_time, &a, &b, &frac)) {
			s->last_vram = a;
			if (xfade)
				draw_blend(s->warp_from_tex, a, tprog, cx, cy);
			else if (interp && b)
				draw_blend(a, b, frac, cx, cy);
			else
				draw_texture(a, cx, cy);
		} else {
			s->underflows.fetch_add(1, std::memory_order_relaxed);
			if (s->last_vram) {
				if (xfade)
					draw_blend(s->warp_from_tex,
						   s->last_vram, tprog, cx, cy);
				else
					draw_texture(s->last_vram, cx, cy);
			}
		}
		return;
	}

	if (disk_mode) {
		/* The disk reader thread keeps the current delayed frame ready. */
		const uint32_t dcx = s->disk ? s->disk->width() : 0;
		const uint32_t dcy = s->disk ? s->disk->height() : 0;
		if (s->disk && dcx == cx && dcy == cy && s->upload_tex) {
			/* Skip the 8 MB copy + GPU upload when the delayed frame
			 * hasn't changed since last render (render fps > source
			 * fps, or paused): the texture still holds it. */
			const uint64_t cur = s->disk->current_ts();
			if (!s->has_uploaded || cur != s->disk_last_upload_ts) {
				uint64_t got = 0;
				if (s->disk->get_current(s->disk_play_buf,
							 &got) &&
				    s->disk_play_buf.size() >=
					    (size_t)cx * cy * 4) {
					upload_frame_texture(
						s->upload_tex,
						s->disk_play_buf.data(), cx, cy);
					s->has_uploaded = true;
					s->disk_last_upload_ts = got;
				}
			}
		} else if (!s->disk) {
			s->underflows.fetch_add(1, std::memory_order_relaxed);
		}
	} else {
		const uint8_t *a = nullptr, *b = nullptr;
		float frac = 0.0f;
		if (s->ring.ready() && s->ring.sample2(target_time, &a, &b, &frac) &&
		    s->upload_tex) {
			const bool will_blend = interp && b && s->upload_tex2;
			/* Skip the GPU upload(s) when the sampled frame (and blend
			 * pair/fraction) is identical to last render - the textures
			 * already hold it (saves up to a full-frame upload/frame at
			 * 1x when the display runs faster than the source). */
			const bool changed =
				!s->has_uploaded || a != s->ram_last_a ||
				(will_blend && (b != s->ram_last_b ||
						frac != s->ram_last_frac));
			if (changed) {
				upload_frame_texture(s->upload_tex, a, cx, cy);
				if (will_blend)
					upload_frame_texture(s->upload_tex2, b,
							     cx, cy);
				s->ram_last_a = a;
				s->ram_last_b = b;
				s->ram_last_frac = frac;
			}
			if (will_blend) {
				ram_blend = true;
				ram_frac = frac;
			}
			s->has_uploaded = true;
		} else {
			s->underflows.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (s->has_uploaded && s->upload_tex) {
		if (xfade)
			draw_blend(s->warp_from_tex, s->upload_tex, tprog, cx, cy);
		else if (ram_blend && s->upload_tex2)
			draw_blend(s->upload_tex, s->upload_tex2, ram_frac, cx, cy);
		else
			draw_texture(s->upload_tex, cx, cy);
	}
}
