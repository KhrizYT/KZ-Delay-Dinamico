/*
 * Broadcast Delay - per-frame capture + time-warp tick (see dse-capture.hpp).
 *
 * capture_frame runs off the OBS render thread every frame (even when the source
 * is hidden) to pre-fill the VRAM/RAM/disk buffer and feed AI detection;
 * dse_video_tick advances the playhead state machine.
 */
#include "dse-capture.hpp"

#include "../audio/dse-audio.hpp"
#include "../core/dse-internal.hpp"
#include "core/dse-constants.hpp"
#include "warp/warp-control.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static void capture_frame(DelayedSource *s)
{
	/* Disabled: detach the target (stop keeping it rendered live + drop audio
	 * callbacks) and free all buffers, so we consume ~nothing. Re-enabling
	 * re-attaches via the retry path below. */
	if (!s->enabled.load(std::memory_order_relaxed)) {
		if (s->ring.ready() || s->stage[0] || s->upload_tex)
			free_video_buffers(s);
		stop_disk(s);
		std::lock_guard<std::mutex> lock(s->target_mutex);
		if (s->target)
			detach_target_locked(s);
		return;
	}

	obs_source_t *target = nullptr;
	{
		std::lock_guard<std::mutex> lock(s->target_mutex);
		/* Retry resolving the target every frame until it loads (fixes
		 * the startup load-order race -- no need to open properties). */
		if (!s->target)
			attach_target_locked(s);
		if (s->target)
			target = obs_source_get_ref(s->target);
	}
	if (!target)
		return;

	/* Recursion guard: target may (mis)include us via a scene loop. */
	if (s->rendering.exchange(true)) {
		obs_source_release(target);
		return;
	}

	uint32_t cx = obs_source_get_width(target);
	uint32_t cy = obs_source_get_height(target);
	if (cx == 0 || cy == 0) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			cx = ovi.base_width;
			cy = ovi.base_height;
		}
	}
	if (cx == 0 || cy == 0) {
		s->rendering.store(false);
		obs_source_release(target);
		return;
	}

	const uint64_t delay_ns = s->delay_ns.load(std::memory_order_relaxed);
	const uint64_t budget = s->budget_bytes.load(std::memory_order_relaxed);
	const size_t frame_bytes = (size_t)cx * cy * 4;
	const int storage = s->storage.load(std::memory_order_relaxed);
	const bool disk_mode = (storage == STORAGE_DISK);
	const bool vram_mode = (storage == STORAGE_VRAM);

	/* Free the backends we are not using. */
	if (!disk_mode)
		stop_disk(s);
	if (!vram_mode && s->vram.ready()) {
		s->vram.destroy();
		s->last_vram = nullptr;
	}
	if (vram_mode && s->ring.ready())
		s->ring.destroy();

	if (vram_mode) {
		/* GPU texture ring, bounded by a conservative VRAM budget
		 * (4 GiB) + GPU-out-of-memory trim. */
		const double fps = active_fps();
		const size_t required = required_capacity(delay_ns, fps);
		const size_t vram_cap_bytes = (size_t)dse::kVramBudgetBytes;
		size_t cap = required;
		size_t budget_cap =
			frame_bytes ? (size_t)(vram_cap_bytes / frame_bytes) : 0;
		if (budget_cap < 2)
			budget_cap = 2;
		if (cap > budget_cap)
			cap = budget_cap;
		if (cap > s->vram_ceiling)
			cap = s->vram_ceiling;

		if (s->vram.configure(cx, cy, cap)) {
			s->frames_buffered.store(0, std::memory_order_relaxed);
			s->last_vram = nullptr;
			s->vram_ceiling = (s->vram.capacity() < cap)
						  ? s->vram.capacity()
						  : SIZE_MAX;
			blog(LOG_INFO,
			     "[broadcast-delay] VRAM buffer: %ux%u x %zu frames (~%.0f MB VRAM)",
			     cx, cy, s->vram.capacity(),
			     (double)frame_bytes * s->vram.capacity() /
				     (1024.0 * 1024.0));
		}
		s->used_bytes.store((uint64_t)frame_bytes * s->vram.capacity(),
				    std::memory_order_relaxed);
	} else if (disk_mode) {
		ensure_gpu_buffers(s, cx, cy);
		/* Disk reader follows the playhead (time-warp). */
		const int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
		const int64_t now_ns = (int64_t)os_gettime_ns();
		const uint64_t reader_delay =
			(now_ns > ph) ? (uint64_t)(now_ns - ph) : 0;
		ensure_disk(s, cx, cy, reader_delay);
	} else {
		ensure_gpu_buffers(s, cx, cy);

		/* Clamp the requested delay to the RAM budget so it never OOMs. */
		const double fps = active_fps();
		const size_t required = required_capacity(delay_ns, fps);
		size_t cap = required;
		size_t budget_cap = frame_bytes ? (size_t)(budget / frame_bytes) : 0;
		if (budget_cap < 2)
			budget_cap = 2;
		if (cap > budget_cap)
			cap = budget_cap;
		if (cap > s->cap_ceiling)
			cap = s->cap_ceiling;

		if (s->ring.configure(cx, cy, cap)) {
			s->frames_buffered.store(0, std::memory_order_relaxed);
			s->stage_primed = false; /* flush */
			if (s->ring.capacity() < cap) {
				s->cap_ceiling = s->ring.capacity();
				blog(LOG_WARNING,
				     "[broadcast-delay] RAM alloc fell short: %zu/%zu frames",
				     s->ring.capacity(), cap);
			}
			if (s->ring.capacity() < required) {
				const double eff =
					(double)s->ring.capacity() /
					(fps > 0 ? fps : 60.0);
				blog(LOG_WARNING,
				     "[broadcast-delay] delay reduced to %.1f s (RAM budget %.1f GB) - switch to Disk for longer delays",
				     eff,
				     (double)budget / (1024.0 * 1024.0 * 1024.0));
			}
			blog(LOG_INFO,
			     "[broadcast-delay] buffer: %ux%u x %zu frames (~%.0f MB RAM)",
			     cx, cy, s->ring.capacity(),
			     (double)frame_bytes * s->ring.capacity() /
				     (1024.0 * 1024.0));
		}
		s->used_bytes.store((uint64_t)frame_bytes * s->ring.capacity(),
				    std::memory_order_relaxed);
	}
	s->cx.store(cx, std::memory_order_relaxed);
	s->cy.store(cy, std::memory_order_relaxed);

	/* Publish the real buffer depth so the playhead can be clamped inside it
	 * (a delay larger than the buffer would otherwise underflow -> stutter). */
	{
		const double fps = active_fps() > 0 ? active_fps() : 60.0;
		size_t cap_frames = vram_mode ? s->vram.capacity()
				    : disk_mode ? s->disk_cap
						: s->ring.capacity();
		/* keep a small margin off the newest/oldest edges */
		const int64_t bn = cap_frames > 6
					   ? (int64_t)((double)(cap_frames - 6) *
						       1.0e9 / fps)
					   : 0;
		s->buffered_ns.store(bn, std::memory_order_relaxed);
	}

	/* Render the target into our texrender. In Docks mode -- and in Scene mode
	 * (which follows the OBS program) -- we render the shared transition source
	 * instead, so scene switches fade/stinger in the delayed feed. Falls back
	 * to the fixed target until the first scene change primes the transition.
	 * Audio still comes from the global program / scene target. */
	const int rmode = s->mode_atomic.load(std::memory_order_relaxed);
	obs_source_t *render_src = target;
	obs_source_t *dock_tr = nullptr;
	if (rmode == MODE_DOCKS || rmode == MODE_SCENE) {
		dock_tr = warp_acquire_dock_render();
		if (dock_tr)
			render_src = dock_tr;
	}

	const uint64_t now = os_gettime_ns();
	gs_texrender_reset(s->texrender);
	bool rendered = false;
	if (gs_texrender_begin(s->texrender, cx, cy)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		obs_source_video_render(render_src);
		gs_texrender_end(s->texrender);
		rendered = true;
	}
	if (dock_tr)
		obs_source_release(dock_tr);

	/* VRAM: copy straight into a ring texture (no readback, no extra copy). */
	if (vram_mode) {
		if (rendered && s->vram.ready()) {
			gs_texture_t *src = gs_texrender_get_texture(s->texrender);
			gs_texture_t *dst = s->vram.acquire_write(now);
			if (src && dst) {
				gs_copy_texture(dst, src);
				s->frames_buffered.fetch_add(1, std::memory_order_relaxed);
			}
		}
		s->rendering.store(false);
		obs_source_release(target);
		return;
	}

	/* Readback (GPU->RAM): stage this frame, copy the one staged last frame.
	 * The one-frame delay lets the GPU finish the copy first, so the map
	 * never stalls the render thread. */
	const bool sink_ready = disk_mode ? (bool)s->disk : s->ring.ready();
	if (rendered && s->stage[0] && s->stage[1] && sink_ready) {
		gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
		const int cur = s->stage_cur;
		const int prev = cur ^ 1;
		if (tex) {
			gs_stage_texture(s->stage[cur], tex);
			s->stage_ts[cur] = now;
		}
		if (s->stage_primed) {
			uint8_t *mapped = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(s->stage[prev], &mapped,
						&linesize)) {
				if (disk_mode) {
					s->disk->push(mapped, linesize,
						      s->stage_ts[prev]);
				} else {
					uint8_t *dst = s->ring.acquire_write(
						s->stage_ts[prev]);
					const uint32_t row = cx * 4;
					if (linesize == row) {
						memcpy(dst, mapped,
						       (size_t)row * cy);
					} else {
						for (uint32_t y = 0; y < cy; y++)
							memcpy(dst + (size_t)y * row,
							       mapped + (size_t)y * linesize,
						       row);
				}
				}
				/* Feed external LIVE taps (Broadcast Censure's AI) the
				 * freshly captured frame - works for every storage
				 * backend; the AI/censure now lives in that plugin. */
				bd_dispatch_live_frame(s, mapped, cx, cy,
						       (uint32_t)linesize,
						       s->stage_ts[prev]);
				gs_stagesurface_unmap(s->stage[prev]);
				s->frames_buffered.fetch_add(1, std::memory_order_relaxed);
		}
	}
	s->stage_cur = prev;
	s->stage_primed = true;
}


	if (disk_mode && s->disk) {
		s->used_bytes.store(s->disk->bytes_on_disk(),
				    std::memory_order_relaxed);
		/* Cache disk health for the dock (read off the UI thread). */
		s->disk_queue.store(s->disk->queue_depth(),
				    std::memory_order_relaxed);
		s->disk_dropped.store(s->disk->dropped_frames(),
				      std::memory_order_relaxed);
	}

	/* Feed any registered public-API video taps (no-op when none). */
	bd_dispatch_video_taps(s);

	s->rendering.store(false);
	obs_source_release(target);
}

void main_render_cb(void *data, uint32_t, uint32_t)
{
	capture_frame(static_cast<DelayedSource *>(data));
}


void dse_video_tick(void *data, float seconds)
{
	auto *s = static_cast<DelayedSource *>(data);
#ifdef _WIN32
	reconcile_hw(s); /* start/stop WASAPI capture from the mixer dock */
#endif
	if (!s->enabled.load(std::memory_order_relaxed))
		return;
	const int64_t now = (int64_t)os_gettime_ns();


	const int64_t target_delay =
		(int64_t)s->delay_ns.load(std::memory_order_relaxed);
	const double dt = seconds > 0.0f ? (double)seconds : 0.0;

	int64_t ph = s->playhead_ns.load(std::memory_order_relaxed);
	if (!s->warp_inited) {
		ph = now - target_delay;
		s->warp_inited = true;
	}

	/* Apply pending manual seeks (+/- buttons). */
	const int64_t seek = s->seek_pending_ns.exchange(0, std::memory_order_relaxed);
	ph += seek;

	/* Minimum delay so the audio read is always a fully-mixed past tick
	 * (reading the current tick would catch only the first source). Kept
	 * small so "Live" still feels real-time, and the video matches it. */
	const int64_t MIN_LIVE_NS = dse::kMinLiveNs;

	int state = s->warp_state.load(std::memory_order_relaxed);
	double a_speed = 1.0;	 /* audio playback speed */
	int64_t a_snap = -1;	 /* >=0 snap audio lag to this delay; -1 free */
	switch (state) {
	case WARP_LIVE:
		ph = now - MIN_LIVE_NS;
		a_speed = 1.0;
		a_snap = MIN_LIVE_NS;
		break;
	case WARP_DELAYED:
		ph = now - target_delay;
		a_speed = 1.0;
		a_snap = target_delay;
		break;
	case WARP_PAUSED: {
		/* Frozen: distance grows. Audio silent (speed 0). Auto-resume
		 * into DELAYED once we have built the achievable delay -- which is
		 * capped by the buffer depth (else the clamp below would stop the
		 * distance short and it would never resume). */
		a_speed = 0.0;
		a_snap = -1;
		const int64_t buf = s->buffered_ns.load(std::memory_order_relaxed);
		int64_t eff = target_delay;
		if (buf > 0 && eff > buf)
			eff = buf;
		if (eff > 0 && (now - ph) >= eff) {
			ph = now - eff;
			s->warp_state.store(WARP_DELAYED, std::memory_order_relaxed);
			a_speed = 1.0;
			a_snap = eff;
		}
		break;
	}
	case WARP_ACCEL: {
		const double sp = s->accel_factor.load(std::memory_order_relaxed);
		if (ph >= now) {
			ph = now;
			s->warp_state.store(WARP_LIVE, std::memory_order_relaxed);
			a_speed = 1.0;
			a_snap = MIN_LIVE_NS;
		} else {
			ph += (int64_t)(dt * 1.0e9 * sp);
			if (ph >= now) {
				ph = now;
				s->warp_state.store(WARP_LIVE,
						    std::memory_order_relaxed);
				a_speed = 1.0;
				a_snap = MIN_LIVE_NS;
			} else {
				a_speed = sp;
				a_snap = -1;
			}
		}
		break;
	}
	case WARP_DECEL: {
		const double sp = s->decel_factor.load(std::memory_order_relaxed);
		/* Cap target to what the buffer can actually hold. */
		const int64_t buf = s->buffered_ns.load(std::memory_order_relaxed);
		int64_t eff = target_delay;
		if (buf > 0 && eff > buf)
			eff = buf;
		const int64_t target_ph = now - eff;
		if (ph <= target_ph) {
			ph = target_ph;
			s->warp_state.store(WARP_DELAYED,
					    std::memory_order_relaxed);
			a_speed = 1.0;
			a_snap = eff;
		} else {
			ph += (int64_t)(dt * 1.0e9 * sp);
			if (ph <= target_ph) {
				ph = target_ph;
				s->warp_state.store(WARP_DELAYED,
						    std::memory_order_relaxed);
				a_speed = 1.0;
				a_snap = eff;
			} else {
				a_speed = sp;
				a_snap = -1;
			}
		}
		break;
	}
	case WARP_PLAY: {
		/* Play at play_speed (0.5/1/2x). The playhead advances at `sp`x,
		 * so the distance from live changes by (1 - sp) per second:
		 * 2x catches up, 0.5x falls back, 1x is constant. */
		const double sp = s->play_speed.load(std::memory_order_relaxed);
		int64_t d = s->play_distance_ns.load(std::memory_order_relaxed);
		d += (int64_t)((1.0 - sp) * dt * 1.0e9);
		if (d < MIN_LIVE_NS)
			d = MIN_LIVE_NS;
		s->play_distance_ns.store(d, std::memory_order_relaxed);
		ph = now - d;
		a_speed = sp;
		a_snap = -1; /* free-run: audio follows the variable rate */
		break;
	}
	}

	if (ph > now)
		ph = now; /* never in the future */
	/* Never go past the oldest buffered frame (else underflow stutter). */
	const int64_t buffered = s->buffered_ns.load(std::memory_order_relaxed);
	const int64_t floor = buffered > 0 ? now - buffered
					   : now - (int64_t)(600ULL * 1000000000ULL);
	if (ph < floor)
		ph = floor;
	s->playhead_ns.store(ph, std::memory_order_relaxed);
	s->last_now_ns.store(now, std::memory_order_relaxed);
	s->audio_speed.store(a_speed, std::memory_order_relaxed);
	s->audio_snap_ns.store(a_snap, std::memory_order_relaxed);
}
